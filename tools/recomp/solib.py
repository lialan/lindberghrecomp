"""
solib.py - lift the shared objects a game dlopens, linked at fixed addresses.

A Lindbergh game does not always link all of its code: Ghost Squad Evolution
dlopens ./snd_util.so (CRI ADX/Sofdec) at run time, and that pulls in
librnalindbergh_jr.so (its OpenAL output). Neither has a fixed address - they
are shared objects - and both are compiled with text relocations, so the call
targets in their code are not known until a dynamic linker has run.

So this is the dynamic linker, run ahead of time: each library gets a fixed
base inside the guest window (above the guest heap, below the stack), their
exports resolve each other's imports, every relocation is applied, and the
relocated bytes are lifted exactly like the main ELF. What no library here
defines - libc, pthreads, OpenAL - gets a token address and becomes a named
hle_call(), the same seam the main game's PLT imports use. The relocated images
are emitted as C arrays; the runtime copies them into place when the game
dlopens them and runs their constructors.

Usage (after `tools recomp` has written the game's own headers):
  python -m tools.recomp.solib <outdir> <lib.so> [<lib.so> ...]
"""

import os
import re
import struct
import sys
import argparse

from elftools.elf.elffile import ELFFile
from capstone.x86 import X86_REG_EBX

from .driver import load_lifter, make_lifter_class, _ident

LIB_BASE   = 0xB4000000          # between the guest heap (ends 0xB0000000) and the stack
TOKEN_BASE = 0xF3000000          # import tokens: dispatch() routes them to hle_call
DATA_ALIGN = 0x1000

R_386_32, R_386_PC32, R_386_GLOB_DAT, R_386_JMP_SLOT, R_386_RELATIVE = 1, 2, 6, 7, 8


class Lib:
    def __init__(self, path, base):
        self.path = path
        self.name = os.path.basename(path)
        self.f = ELFFile(open(path, "rb"))
        self.base = base
        loads = [s for s in self.f.iter_segments() if s["p_type"] == "PT_LOAD"]
        lo = min(s["p_vaddr"] for s in loads) & ~0xFFF
        hi = max(s["p_vaddr"] + s["p_memsz"] for s in loads)
        if lo != 0:
            raise SystemExit("%s: first PT_LOAD is not at 0" % path)
        self.size = (hi + 0xFFF) & ~0xFFF
        self.img = bytearray(self.size)
        for s in loads:
            data = s.data()
            self.img[s["p_vaddr"]:s["p_vaddr"] + len(data)] = data
        self.dynsym = self.f.get_section_by_name(".dynsym")
        dyn = self.f.get_section_by_name(".dynamic")
        tags = {t.entry.d_tag: t.entry.d_val for t in dyn.iter_tags()}
        self.init = tags.get("DT_INIT")
        self.pic_tables = []          # (lo, hi): switch tables inside .text
        self.fini = tags.get("DT_FINI")
        self.init_array = (tags.get("DT_INIT_ARRAY"), tags.get("DT_INIT_ARRAYSZ", 0))
        self.exports = {}
        for s in self.dynsym.iter_symbols():
            if s["st_shndx"] != "SHN_UNDEF" and s.name and s["st_info"]["bind"] != "STB_LOCAL":
                self.exports[s.name] = base + s["st_value"]

    def functions(self):
        """Every function symbol, plus DT_INIT/DT_FINI. CRT pieces (_init,
        frame_dummy, the ctor walkers) carry no size, so a zero-sized symbol
        runs to the next symbol in its section."""
        secs = [x for x in self.f.iter_sections()
                if x["sh_flags"] & 0x4 and x["sh_size"]]          # SHF_EXECINSTR
        def section_end(v):
            for x in secs:
                if x["sh_addr"] <= v < x["sh_addr"] + x["sh_size"]:
                    return x["sh_addr"] + x["sh_size"]
            return None
        starts, named, reach = set(), {}, {}
        for sec in (self.f.get_section_by_name(".symtab"), self.dynsym):
            if not sec:
                continue
            for sym in sec.iter_symbols():
                if sym["st_shndx"] == "SHN_UNDEF" or sym["st_info"]["type"] not in ("STT_FUNC", "STT_NOTYPE"):
                    continue
                v = sym["st_value"]
                if section_end(v) is None:
                    continue
                starts.add(v)
                if sym["st_info"]["type"] == "STT_FUNC" or sym.name in ("_init", "_fini"):
                    named.setdefault(v, (sym["st_size"], sym.name))
        for v, nm in ((self.init, "_init"), (self.fini, "_fini")):
            if v:
                named.setdefault(v, (0, nm))
                starts.add(v)
        if True:
            import importlib.util
            _dis_py = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "pcrecomp", "tools", "disasm", "disasm32.py")
            _spec = importlib.util.spec_from_file_location("disasm32", _dis_py)
            _mod = importlib.util.module_from_spec(_spec)
            _spec.loader.exec_module(_mod)
            Disassembler = _mod.Disassembler
            text = self.f.get_section_by_name(".text")
            if text:
                t_lo = self.base + text["sh_addr"]
                t_hi = t_lo + text["sh_size"]
                seeds = {t_lo}
                for v in starts:
                    if t_lo <= self.base + v < t_hi:
                        seeds.add(self.base + v)
                for sec_name in (".ctors", ".dtors", ".init_array", ".fini_array", ".data.rel.ro", ".data", ".rodata"):
                    sec = self.f.get_section_by_name(sec_name)
                    if not sec:
                        continue
                    s_off, s_sz = sec["sh_addr"], sec["sh_size"]
                    for off in range(s_off, s_off + s_sz - 3, 4):
                        val = self.rd32(off)
                        if t_lo <= val < t_hi:
                            seeds.add(val)
                for sec_name in (".init", ".fini"):
                    sec = self.f.get_section_by_name(sec_name)
                    if not sec:
                        continue
                    s_off, s_sz = sec["sh_addr"], sec["sh_size"]
                    for off in range(s_off, s_off + s_sz - 4):
                        if self.img[off] == 0xE8:
                            rel = struct.unpack_from("<i", self.img, off + 1)[0]
                            tgt = (self.base + off + 5 + rel) & 0xFFFFFFFF
                            if t_lo <= tgt < t_hi:
                                seeds.add(tgt)
                import types
                dis_secs = [types.SimpleNamespace(
                    name=sec.name,
                    is_code=(sec.name == ".text"),
                    virtual_address=sec["sh_addr"],
                    virtual_size=sec["sh_size"],
                    raw_offset=sec["sh_addr"],
                    raw_size=sec["sh_size"],
                ) for sec in self.f.iter_sections() if sec["sh_addr"] and sec["sh_size"]]
                dis = Disassembler(bytes(self.img), self.base, dis_secs)
                # PIC code holds the GOT in ebx: its switch tables are GOT-relative
                got = self.f.get_section_by_name(".got")
                if got is not None:
                    dis.pic_base = self.base + got["sh_addr"]
                found = dis.find_functions(t_lo, t_hi, dict(self.plt_stubs()), seeds)
                # The call scan reads every E8 byte as a call, so some
                # candidates start inside another instruction (the e8 of
                # `mov [edx+0xe8], 0`). Each start ends the function before it,
                # so one of those cut libCg's 0x1467dc short mid-body. Keep a
                # start only if no kept function, walking upward, decoded an
                # instruction across it. Symbols are always kept.
                inside = bytearray(t_hi - t_lo)
                kept, dropped = [], 0
                for va in sorted(found):
                    if va - self.base not in named and inside[va - t_lo]:
                        dropped += 1
                        continue
                    kept.append(va)
                    for lo_, hi_ in getattr(found[va], "pic_tables", ()):
                        for k in range(max(lo_, t_lo), min(hi_, t_hi)):
                            inside[k - t_lo] = 1
                    for b in found[va].blocks.values():
                        for ins in b.instructions:
                            for k in range(ins.address + 1, ins.address + ins.size):
                                if t_lo <= k < t_hi:
                                    inside[k - t_lo] = 1
                if dropped:
                    print("[solib] %s: %d candidate starts inside another instruction dropped"
                          % (self.name, dropped), file=sys.stderr)
                # Switch arms: `jmp eax` is dispatched at run time, so each arm
                # needs an entry of its own (overlapping the switch's function,
                # as alias entries do).
                arms = set()
                for va in kept:
                    arms |= found[va].jump_targets
                arms -= set(kept)
                for a in sorted(arms):
                    fa = found.get(a) or dis.disassemble_function(a, dict(self.plt_stubs()))
                    if fa and fa.blocks:
                        found[a] = fa
                        kept.append(a)
                if arms:
                    print("[solib] %s: %d switch arms made entries" % (self.name, len(arms)), file=sys.stderr)
                # A branch out of a body's own range (an arm's loop back to the
                # switch above it, libCg 0xf7a12 -> 0xf7854) is dispatched, so
                # its target needs an entry too; those bodies can branch out
                # again, hence the fixpoint.
                have = set(kept)
                todo = list(kept)
                extra = 0
                while todo:
                    va = todo.pop()
                    f = found[va]
                    end = max(i.address + i.size for b in f.blocks.values() for i in b.instructions)
                    for b in f.blocks.values():
                        for ins in b.instructions:
                            if ins.is_cond_jump or ins.is_uncond_jump:
                                tgt = ins.get_branch_target()
                                if tgt is None or va <= tgt < end:
                                    continue
                            elif (dis.pic_base is not None and ins.mnemonic == "lea"
                                  and len(ins.operands or ()) == 2
                                  and ins.operands[1].mem.base == X86_REG_EBX
                                  and not ins.operands[1].mem.index):
                                # a callback's address: `lea eax, [ebx + f@GOTOFF]`
                                # (libCg 0xe5905 -> 0xe5f48, which opens with no
                                # standard prologue)
                                tgt = (dis.pic_base + ins.operands[1].mem.disp) & 0xFFFFFFFF
                            else:
                                continue
                            if tgt in have or not (t_lo <= tgt < t_hi):
                                continue
                            ft = found.get(tgt) or dis.disassemble_function(tgt, dict(self.plt_stubs()))
                            have.add(tgt)
                            if ft and ft.blocks:
                                found[tgt] = ft
                                kept.append(tgt)
                                todo.append(tgt)
                                extra += 1
                if extra:
                    print("[solib] %s: %d branch and callback targets outside their body made entries" % (self.name, extra),
                          file=sys.stderr)
                for va in kept:
                    self.pic_tables += getattr(found[va], "pic_tables", [])
                for va in kept:
                    rva = va - self.base
                    starts.add(rva)
                    named.setdefault(rva, (0, "sub_%08X" % va))
                    # how far its own control flow reaches. Entries into the
                    # middle of a function (shared tails, loop heads) are
                    # starts too, and ending a body at the next start left a
                    # branch past it (libCg 0xfd60c) with nowhere to go. The
                    # bodies overlap instead, as alias entries are meant to.
                    reach[rva] = max(ins.address + ins.size
                                     for b in found[va].blocks.values()
                                     for ins in b.instructions) - self.base
        order = sorted(starts)
        out = {}
        for v, (size, name) in named.items():
            end = section_end(v)
            if not size and v in reach:
                size = min(reach[v], end) - v
            if not size:
                later = [x for x in order if v < x < end]
                size = (later[0] if later else end) - v
            out[self.base + v] = (size, name)
        return out

    def plt_stubs(self):
        """.plt stub address -> import symbol name, in .rel.plt order."""
        plt = self.f.get_section_by_name(".plt")
        relplt = self.f.get_section_by_name(".rel.plt")
        if not plt or not relplt:
            return {}
        out = {}
        for i, r in enumerate(relplt.iter_relocations()):
            sym = self.dynsym.get_symbol(r["r_info_sym"])
            out[self.base + plt["sh_addr"] + 16 * (i + 1)] = sym.name
        return out

    def rd32(self, off):
        return struct.unpack_from("<I", self.img, off)[0]

    def wr32(self, off, v):
        struct.pack_into("<I", self.img, off, v & 0xFFFFFFFF)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("outdir")
    parser.add_argument("--main-elf", help="resolve callbacks and data exported by the main executable")
    parser.add_argument("libraries", nargs="+")
    opts = parser.parse_intermixed_args()
    outdir, paths = opts.outdir, opts.libraries

    libs, base = [], LIB_BASE
    for p in paths:
        lib = Lib(p, base)
        libs.append(lib)
        base = (base + lib.size + 0xFFFF) & ~0xFFFF
    data_base = base                      # zeroed objects for data imports
    exports = {}
    for lib in libs:
        for n, a in lib.exports.items():
            exports.setdefault(n, a)

    # ELF symbol interposition: the executable's definitions take priority.
    # Libraries can call back into its CRI decoder and mutex helpers, and
    # reference the RTTI objects copied there by the dynamic linker.
    resolved = dict(exports)
    if opts.main_elf:
        with open(opts.main_elf, "rb") as stream:
            dynsym = ELFFile(stream).get_section_by_name(".dynsym")
            if dynsym:
                for sym in dynsym.iter_symbols():
                    if sym.name and sym["st_shndx"] != "SHN_UNDEF":
                        resolved[sym.name] = sym["st_value"]

    # what the main ELF already imports: its HLE ids exist already
    main_imports = set(re.findall(r'X\(HLE_\w+, "([^"]+)"\)',
                                  open(os.path.join(outdir, "recomp_imports.h")).read()))

    tokens, data_objs = {}, {}

    def resolve(lib, symidx):
        if symidx == 0:
            return 0
        s = lib.dynsym.get_symbol(symidx)
        if s["st_shndx"] != "SHN_UNDEF":
            return lib.base + s["st_value"]
        if s.name in resolved:
            return resolved[s.name]
        if s["st_info"]["bind"] == "STB_WEAK":
            return 0
        if s["st_info"]["type"] == "STT_OBJECT":
            if s.name not in data_objs:
                data_objs[s.name] = data_base + 64 * len(data_objs)
            return data_objs[s.name]
        if s.name not in tokens:
            tokens[s.name] = TOKEN_BASE + 16 * len(tokens)
        return tokens[s.name]

    counts = {}
    for lib in libs:
        for sec in lib.f.iter_sections():
            if sec["sh_type"] != "SHT_REL":
                continue
            for r in sec.iter_relocations():
                t, off = r["r_info_type"], r["r_offset"]
                P = lib.base + off
                A = lib.rd32(off)
                if t == R_386_RELATIVE:
                    lib.wr32(off, lib.base + A)
                elif t == R_386_32:
                    lib.wr32(off, resolve(lib, r["r_info_sym"]) + A)
                elif t == R_386_PC32:
                    lib.wr32(off, resolve(lib, r["r_info_sym"]) + A - P)
                elif t in (R_386_GLOB_DAT, R_386_JMP_SLOT):
                    lib.wr32(off, resolve(lib, r["r_info_sym"]))
                else:
                    raise SystemExit("%s: relocation type %d at %#x" % (lib.name, t, off))
                counts[t] = counts.get(t, 0) + 1
    print("[solib] %s relocated %s, %d import tokens, %d data imports"
          % (" ".join(l.name for l in libs), counts, len(tokens), len(data_objs)), file=sys.stderr)

    # call targets the lifter must not follow: import tokens (by name) and the
    # libraries' own PLT stubs, which go to a token or to the other library
    plt_names = {a: n for n, a in tokens.items()}
    alias = {}
    for lib in libs:
        for stub, name in lib.plt_stubs().items():
            target = resolved.get(name)
            if target is not None:
                alias[stub] = target
            elif name in tokens:
                plt_names[stub] = name

    lift = load_lifter()
    lo = libs[0].base
    hi = libs[-1].base + libs[-1].size
    lift.IMAGE_BASE = lo

    def read_va(va, n):
        for lib in libs:
            if lib.base <= va < lib.base + lib.size:
                o = va - lib.base
                return bytes(lib.img[o:o + n])
        return b"\0" * n

    base_cls = make_lifter_class(lift, plt_names)

    class SoLifter(base_cls):
        def translate(self, insn, labels):
            from capstone.x86 import X86_OP_IMM
            if insn.mnemonic in ("call", "jmp") and insn.operands:
                op = insn.operands[0]
                if op.type == X86_OP_IMM and op.imm in alias:
                    if insn.mnemonic == "call":
                        return ["push32(c, 0x%08Xu); dispatch(c, 0x%08Xu);"
                                % (insn.address + insn.size, alias[op.imm])]
                    return ["dispatch_jmp(c, 0x%08Xu); return;" % alias[op.imm]]
                if insn.mnemonic == "jmp" and op.type == X86_OP_IMM and op.imm in plt_names:
                    # tail call into an import: the caller's return address is
                    # already on the stack, which is what an indirect PLT hit
                    # expects
                    return ["dispatch_jmp(c, 0x%08Xu); return;" % op.imm]
            return super().translate(insn, labels)

    lifter = SoLifter(paths[0], hi - lo, read_va)

    funcs = {}
    for lib in libs:
        funcs.update(lib.functions())

    # The lifter decodes a body linearly, so a switch table inside it (GCC
    # PIC puts them right after the jmp) decodes as junk, falls out of step
    # with the real instructions after it, and their labels are lost (libCg
    # 0xd8978's je 0xdb178). It gets the tables as nops instead; the image
    # the game runs keeps them.
    tables = sorted({t for lib in libs for t in lib.pic_tables})
    def code_bytes(va, n):
        b = bytearray(read_va(va, n))
        for lo_, hi_ in tables:
            if lo_ < va + n and hi_ > va:
                for k in range(max(lo_, va), min(hi_, va + n)):
                    b[k - va] = 0x90
        return bytes(b)
    header = ["/* AUTO-GENERATED by lindberghrecomp tools/recomp/solib.py - do not edit */",
              '#define RECOMP_LIFTED 1   /* cpu.h: the guest window is fixed by now */',
              '#include "lindbergh_rt.h"', ""]
    chunks, cur, done = [], list(header), []
    for va in sorted(funcs):
        size, name = funcs[va]
        cur.append("/* %s */" % name)
        cur.append(lifter.lift_function(code_bytes(va, size), va))
        cur.append("")
        done.append(va)
        if len(done) % 400 == 0:
            chunks.append(cur)
            cur = list(header)
    if len(cur) > len(header):
        chunks.append(cur)
    # The main ELF's passes (driver.py): direct calls between the functions
    # lifted here, dead flags dropped, registers in locals. Without them the
    # sound libraries' decoders ran several times slower on the game thread.
    from .flags import strip_dead_flags
    from .regcache import cache_registers, PRELUDE as REGCACHE_PRELUDE
    lifted = set(done)
    lit = re.compile(r"\bdispatch\(c, 0x([0-9A-F]{8})u\);")

    def direct(m):
        va = int(m.group(1), 16)
        return "DISPATCH_TO(c, 0x%su, L_%s);" % (m.group(1), m.group(1)) if va in lifted else m.group(0)

    flags_dropped = 0
    for i, chunk in enumerate(chunks):
        body = lit.sub(direct, "\n".join(chunk[len(header):]))
        body, n = strip_dead_flags(body)
        flags_dropped += n
        body, _ = cache_registers(body)
        protos = sorted(set(re.findall(r"DISPATCH_TO\(c, 0x[0-9A-F]{8}u, (L_[0-9A-F]{8})\)", body)))
        with open(os.path.join(outdir, "recomp_solib_funcs_%04d.c" % i), "w") as f:
            f.write("\n".join(header) + "\n" + REGCACHE_PRELUDE)
            f.write("".join("void %s(CPU *c);\n" % p for p in protos) + "\n")
            f.write(body)

    new_imports = sorted(n for n in tokens if n not in main_imports)
    with open(os.path.join(outdir, "recomp_solib.h"), "w") as f:
        f.write("/* AUTO-GENERATED by tools/recomp/solib.py: the dlopened libraries. */\n")
        f.write("#define SOLIB_FUNCS(X) \\\n" +
                " \\\n".join("    X(%08X)" % a for a in done) + "\n")
        f.write("#define SOLIB_IMPORTS(X) \\\n" +
                (" \\\n".join('    X(HLE_%s, "%s")' % (_ident(n), n) for n in new_imports) or "") + "\n")
        f.write("#define SOLIB_PLT(X) \\\n" +
                " \\\n".join("    X(0x%08Xu, HLE_%s)" % (a, _ident(n))
                             for a, n in sorted(plt_names.items())) + "\n")
        f.write("#define SOLIB_ALIASES(X) \\\n" +
                (" \\\n".join("    X(0x%08Xu, 0x%08Xu)" % (s, t) for s, t in sorted(alias.items())) or "") + "\n")
        f.write("#define SOLIB_EXPORTS(X) \\\n" +
                " \\\n".join('    X("%s", 0x%08Xu)' % (n, a) for n, a in sorted(exports.items())) + "\n")
        # the data imports no library defines, each a zeroed slot: the
        # runtime fills the ones it knows (libCg's __ctype_b and friends,
        # the pointers an old glibc exported in place of __ctype_b_loc)
        f.write("#define SOLIB_DATA(X) \\\n" +
                (" \\\n".join('    X("%s", 0x%08Xu)' % (n, a) for n, a in sorted(data_objs.items())) or "") + "\n")
        f.write("#define SOLIB_DATA_BASE 0x%08Xu\n#define SOLIB_DATA_SIZE 0x%Xu\n"
                % (data_base, max(DATA_ALIGN, 64 * len(data_objs))))
    with open(os.path.join(outdir, "recomp_solib_img.c"), "w") as f:
        f.write("/* AUTO-GENERATED: the relocated library images. Game code - never distribute. */\n")
        f.write('#include "solib.h"\n')
        for i, lib in enumerate(libs):
            f.write("static const unsigned char img%d[%d] = {\n" % (i, lib.size))
            b = lib.img
            for o in range(0, len(b), 32):
                f.write(",".join(str(x) for x in b[o:o + 32]) + ",\n")
            f.write("};\n")
        f.write("const SolibImage g_solib_images[] = {\n")
        for i, lib in enumerate(libs):
            ia, isz = lib.init_array
            f.write('    { "%s", 0x%08Xu, %d, img%d, 0x%08Xu, 0x%08Xu, %d },\n'
                    % (lib.name, lib.base, lib.size, i,
                       lib.base + lib.init if lib.init else 0,
                       lib.base + ia if ia else 0, isz // 4))
        f.write("};\nconst int g_solib_count = %d;\n" % len(libs))
    print("[solib] lifted %d functions in %d units, %d new imports: %s; %d instructions' flags dropped as dead"
          % (len(done), len(chunks), len(new_imports), " ".join(new_imports), flags_dropped), file=sys.stderr)


if __name__ == "__main__":
    main()
