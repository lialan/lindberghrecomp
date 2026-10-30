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

from elftools.elf.elffile import ELFFile

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
        starts, named = set(), {}
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
        order = sorted(starts)
        out = {}
        for v, (size, name) in named.items():
            if not size:
                end = section_end(v)
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
    outdir, paths = sys.argv[1], sys.argv[2:]
    if not paths:
        raise SystemExit(__doc__)

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
        if s.name in exports:
            return exports[s.name]
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
            target = exports.get(name)
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
    header = ["/* AUTO-GENERATED by lindberghrecomp tools/recomp/solib.py - do not edit */",
              '#define RECOMP_LIFTED 1   /* cpu.h: the guest window is fixed by now */',
              '#include "lindbergh_rt.h"', ""]
    chunks, cur, done = [], list(header), []
    for va in sorted(funcs):
        size, name = funcs[va]
        cur.append("/* %s */" % name)
        cur.append(lifter.lift_function(read_va(va, size), va))
        cur.append("")
        done.append(va)
        if len(done) % 400 == 0:
            chunks.append(cur)
            cur = list(header)
    if len(cur) > len(header):
        chunks.append(cur)
    for i, chunk in enumerate(chunks):
        with open(os.path.join(outdir, "recomp_solib_funcs_%04d.c" % i), "w") as f:
            f.write("\n".join(chunk))

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
    print("[solib] lifted %d functions in %d units, %d new imports: %s"
          % (len(done), len(chunks), len(new_imports), " ".join(new_imports)), file=sys.stderr)


if __name__ == "__main__":
    main()
