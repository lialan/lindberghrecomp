#!/usr/bin/env python3
"""
bounds.py - function bounds (and names) for a stripped game ELF.

The driver lifts the functions an ELF's symbol table lists. A shipped game is
sometimes stripped - The House of the Dead 4 Special is - and then there is no
list. This builds one:

  * pcrecomp's recursive function finder (the one stripped Windows games go
    through), run over the ELF's code sections, decides where functions are;
  * .eh_frame seeds it. g++ writes an unwind record for every function it
    compiles, with the exact start and length, and stripping keeps the section
    (exceptions need it). Hand-written and C-compiled code has none, which is
    what the finder is for;
  * a reference ELF of the same engine that does have symbols (the original
    House of the Dead 4 for the Special) names what it can: a function whose
    bytes match one of the reference's, addresses masked out, takes that name,
    and the callees of a named pair are paired up call by call. The runtime
    finds a few guest functions by name (overrides, statically linked
    libraries), so names are not decoration.

Output is one function per line, `0xADDR size [name]`, which `tools recomp
--bounds` reads.

Usage:
  py -3.11 -m tools bounds <game.elf> <out.txt> [--ref <reference.elf>] [--names <table>]

--names applies a fixed `0xADDR name` table instead, which is how a game's
repo carries the few names its runtime needs without the reference ELF.
"""

import bisect
import hashlib
import struct
import sys
from collections import defaultdict

from ..elf.elf32 import Elf32
from .driver import _ROOT

import importlib.util
import os


def _load_disasm():
    path = os.path.join(_ROOT, "pcrecomp", "tools", "disasm", "disasm32.py")
    spec = importlib.util.spec_from_file_location("disasm32", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---- .eh_frame --------------------------------------------------------------

def _uleb(d, o):
    r = s = 0
    while True:
        b = d[o]
        o += 1
        r |= (b & 0x7F) << s
        s += 7
        if b < 0x80:
            return r, o


def _read_enc(d, o, enc, va):
    """A DW_EH_PE-encoded pointer at file offset o (whose address is va)."""
    fmt, app = enc & 0x0F, enc & 0x70
    if fmt in (0x00, 0x03, 0x0B):
        v, n = struct.unpack_from("<i" if fmt == 0x0B else "<I", d, o)[0], 4
    elif fmt in (0x02, 0x0A):
        v, n = struct.unpack_from("<h" if fmt == 0x0A else "<H", d, o)[0], 2
    else:
        raise ValueError("pointer encoding %#x" % enc)
    if app == 0x10:
        v += va
    return v & 0xFFFFFFFF, o + n


def eh_frame_functions(elf):
    """{start: length} from .eh_frame's FDEs; {} if the ELF has none or the
    section is in a shape this does not read."""
    s = elf.section(".eh_frame")
    if not s or not s.size:
        return {}
    d, base, end = elf.data, s.offset, s.offset + s.size
    cies, out, o = {}, {}, base
    try:
        while o < end:
            ln = struct.unpack_from("<I", d, o)[0]
            if ln == 0:
                break
            st, nxt = o + 4, o + 4 + ln
            cid = struct.unpack_from("<I", d, st)[0]
            if cid == 0:                        # a CIE: only its FDE encoding matters
                p = st + 5
                aug = d[p:d.index(b"\0", p)].decode("latin-1")
                p += len(aug) + 1
                _, p = _uleb(d, p)              # code alignment
                while d[p] & 0x80:              # data alignment (sleb)
                    p += 1
                p += 2                          # ...and the return-address column
                fenc = 0
                if aug.startswith("z"):
                    _, p = _uleb(d, p)
                    for ch in aug[1:]:
                        if ch == "R":
                            fenc = d[p]
                            p += 1
                        elif ch == "L":
                            p += 1
                        elif ch == "P":
                            penc = d[p]
                            p += 1
                            _, p = _read_enc(d, p, penc, 0)
                cies[o] = fenc
            else:
                fenc = cies[st - cid]
                p = st + 4
                loc, p = _read_enc(d, p, fenc, s.addr + (p - base))
                rng, _ = _read_enc(d, p, fenc & 0x0F, 0)
                if loc and rng:
                    out[loc] = rng
            o = nxt
    except (ValueError, KeyError, IndexError) as e:
        print("[!] .eh_frame: %s; using the %d FDEs read so far" % (e, len(out)),
              file=sys.stderr)
    return out


# ---- the finder over an ELF ---------------------------------------------------

class _Sec:
    """What pcrecomp's Disassembler wants of a section (it was written for PE)."""
    def __init__(self, name, rva, size, is_code):
        self.name = name
        self.virtual_address = rva
        self.virtual_size = size
        self.raw_offset = rva            # the flat image below is laid out by RVA
        self.raw_size = size
        self.is_code = is_code


def find_functions(elf, plt):
    """{addr: size} for every function the finder recovers, seeded with the
    entry point, .eh_frame's starts and every PLT-free code section's start."""
    dis = _load_disasm()
    base = elf.image_base
    image = bytearray(elf.image_size)
    for seg in elf.loads:
        n = min(seg.filesz, seg.memsz)
        image[seg.vaddr - base:seg.vaddr - base + n] = elf.data[seg.offset:seg.offset + n]

    skip = (".plt", ".plt.got", ".plt.sec", ".iplt")
    secs, code = [], []
    for sh in elf.sections:
        if not sh.addr or not sh.size or not sh.flags & 0x2:      # SHF_ALLOC
            continue
        if sh.type == 8:                                          # .bss: no bytes
            continue
        # Unwind and exception tables, symbol and relocation tables: every
        # function pointer they hold is one .eh_frame already seeds, and
        # read unaligned they are full of words that merely look like code
        # addresses (an odd offset in .gcc_except_table made 0x08270000 a
        # "function" whose garbage decode then hid the real one at 0x08270010).
        if sh.name.startswith((".eh_frame", ".gcc_except_table", ".rel", ".dyn",
                               ".hash", ".gnu", ".note", ".interp")):
            continue
        is_code = bool(sh.flags & 0x4) and sh.name not in skip
        secs.append(_Sec(sh.name, sh.addr - base, sh.size, is_code))
        if is_code:
            code.append((sh.addr, sh.addr + sh.size))

    d = dis.Disassembler(bytes(image), base, secs)
    d.md.detail = True
    fde = eh_frame_functions(elf)
    seeds = set(fde) | {elf.entry} | {lo for lo, _ in code}
    lo, hi = min(c[0] for c in code), max(c[1] for c in code)
    # The finder's PLT knowledge is a PE import table; here a call to a PLT
    # stub is just a call outside the code sections, which it already skips.
    found = d.find_functions(lo, hi, {}, seeds=sorted(s for s in seeds if lo <= s < hi))
    out = {}
    for a, f in found.items():
        size = f.size or (f.end - a)
        # An unwind record is exact where the finder guessed: it knows nothing
        # of a function's cold tail or a switch table's far arms.
        if a in fde and fde[a] >= size:
            size = fde[a]
        if size > 0:
            out[a] = size
    # A conditional jump into another function (Intel's memset dispatcher picks
    # a variant that way) is a branch the finder does not follow; the lifter
    # dispatches it, so its target needs a body of its own.
    close_all(dis, d, elf.read_va, out, lo, hi)
    keep_unwind_starts(fde, out, lo, hi)
    close_pointer_tables(elf, out, lo, hi)
    close_all(dis, d, elf.read_va, out, lo, hi)
    keep_unwind_starts(fde, out, lo, hi)
    fix_cut_instructions(elf.read_va, out, lo, hi)
    print("[*] %d functions (%d from .eh_frame)" % (len(out), len(set(fde) & set(out))),
          file=sys.stderr)
    return out


def close_jump_tables(d, funcs, lo, hi):
    """A switch's case arms outside its function's extent become entries.

    Clamping cuts a function at the next start the finder found, and in C code
    with no unwind record that is often a label inside it - a loop head the
    arms jump back to. The arms past it are then neither in the switch's
    extent nor entries, and the switch's dispatch to them finds nothing."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    added = 0
    for _ in range(3):
        new = {}
        starts = sorted(funcs)
        for a, sz in list(funcs.items()):
            try:
                code = elf_read(d, a, sz)
            except ValueError:
                continue
            for ins in md.disasm(code, a):
                if ins.mnemonic != "jmp" or ins.bytes[0] != 0xFF:
                    continue
                for t in d.jump_table_targets(ins):
                    if not (a <= t < a + sz) and t not in funcs and lo <= t < hi:
                        i = bisect.bisect_right(starts, t)
                        new[t] = (starts[i] if i < len(starts) else hi) - t
        if not new:
            break
        funcs.update(new)
        added += len(new)
    if added:
        print("[*] Added %d switch arms outside their function" % added, file=sys.stderr)


def close_pointer_tables(elf, funcs, lo, hi):
    """Aligned runs of code pointers in .rodata and .data - vtables, callback
    and switch tables - name entries the rest may have dropped or never seen.
    Each one that lands on an instruction boundary of the extent holding it
    becomes an entry; one that lands mid-instruction is data that only looks
    like an address."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    starts = sorted(funcs)
    want = set()
    for name in (".rodata", ".data"):
        s = elf.section(name)
        if not s:
            continue
        d = elf.read_va(s.addr, s.size)
        w = [struct.unpack_from("<I", d, i)[0] for i in range(0, len(d) - 3, 4)]
        for i, x in enumerate(w):
            if not (lo <= x < hi) or x in funcs:
                continue
            if (i and lo <= w[i - 1] < hi) or (i + 1 < len(w) and lo <= w[i + 1] < hi):
                want.add(x)
    added = 0
    for x in sorted(want):
        i = bisect.bisect_right(starts, x) - 1
        if i < 0:
            continue
        a = starts[i]
        if x >= a + funcs[a]:
            continue
        ends = set()
        for ins in md.disasm(elf.read_va(a, funcs[a]), a):
            ends.add(ins.address)
        if x in ends:
            j = bisect.bisect_right(starts, x)
            funcs[x] = (starts[j] if j < len(starts) else hi) - x
            added += 1
    if added:
        print("[*] Added %d entries from pointer tables" % added, file=sys.stderr)


def keep_unwind_starts(fde, funcs, lo, hi):
    """An unwind record is the compiler saying a function starts there, so no
    heuristic gets to drop one: a garbage decode from a bogus neighbour can
    make a real start look like the inside of an instruction (0x080ACC9E, a
    virtual method, went that way)."""
    starts = sorted(funcs)
    back = 0
    for a in sorted(fde):
        if a in funcs or not (lo <= a < hi):
            continue
        i = bisect.bisect_right(starts, a)
        funcs[a] = (starts[i] if i < len(starts) else hi) - a
        back += 1
    if back:
        print("[*] Restored %d unwind-recorded starts" % back, file=sys.stderr)


def close_all(dis, d, read_va, funcs, lo, hi):
    """Each pass can add entries whose own branches the others have not seen:
    run them until the catalog stops changing."""
    dis.close_dispatch_targets(read_va, funcs, lo, hi)
    drop_mid_instruction(dis, read_va, funcs, lo, hi)
    for _ in range(8):
        before = dict(funcs)
        dis.close_dispatch_targets(read_va, funcs, lo, hi, verbose=False)
        close_jump_tables(d, funcs, lo, hi)
        fix_cut_instructions(read_va, funcs, lo, hi)
        if funcs == before:
            break


def drop_mid_instruction(dis, read_va, funcs, lo, hi):
    """The passes above add entries after the finder's own clean up, and a
    branch decoded out of data can land inside a real instruction (0x083BAC53,
    the second byte of an idiv). An extent ending there cuts the instruction in
    two. Drop such entries again, and run what ended at one on to the next."""
    sizes = dict(funcs)
    dis.drop_mid_instruction_entries(read_va, sizes, lo, hi)
    gone = set(funcs) - set(sizes)
    if not gone:
        return
    starts = sorted(sizes)
    for a in starts:
        end = a + sizes[a]
        if end in gone:
            i = bisect.bisect_right(starts, a)
            sizes[a] = (starts[i] if i < len(starts) else hi) - a
    funcs.clear()
    funcs.update(sizes)


def fix_cut_instructions(read_va, funcs, lo, hi):
    """Every extent ends on one of its own instruction boundaries.

    The lifter reads an extent linearly and, at its end, falls through to
    whatever starts there. An extent cut by a start the finder believed in
    mid-instruction (0x083BAC53 is the second byte of an idiv) loses the
    instruction and falls through to an address that is nobody's. Extents may
    overlap, so the fix is local: run the extent over the cut instruction and
    make the address after it an entry of its own."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.skipdata = False
    fixed = 0
    for _ in range(4):
        new = {}
        starts = sorted(funcs)
        for a in starts:
            sz = funcs[a]
            try:
                code = read_va(a, sz + 16)
            except ValueError:
                continue
            end = a
            for ins in md.disasm(code, a):
                if ins.address >= a + sz:
                    break
                end = ins.address + ins.size
            if end > a + sz and lo <= end < hi:
                funcs[a] = end - a
                if end not in funcs:
                    i = bisect.bisect_right(starts, end)
                    new[end] = (starts[i] if i < len(starts) else hi) - end
                fixed += 1
        if not new:
            break
        funcs.update(new)
    if fixed:
        print("[*] Ran %d extents over an instruction they cut" % fixed, file=sys.stderr)


def elf_read(d, a, n):
    b = d.read_bytes(a, n)
    if b is None:
        raise ValueError(hex(a))
    return b


# ---- names from a reference ELF ---------------------------------------------

def _masked(md, code, va, lo, hi):
    """The function's bytes with everything address-shaped zeroed: relative
    call/jmp displacements, and immediates or displacements that point into
    the image [lo, hi). What is left changes only if the code does."""
    out = bytearray(code)
    calls = []
    for ins in md.disasm(code, va):
        o = ins.address - va
        b = ins.bytes
        if b[0] in (0xE8, 0xE9) and ins.size == 5:
            out[o + 1:o + 5] = b"\0\0\0\0"
            if b[0] == 0xE8:
                calls.append((o, (ins.address + 5 + struct.unpack_from("<i", b, 1)[0]) & 0xFFFFFFFF))
            continue
        if b[0] == 0x0F and len(b) == 6 and 0x80 <= b[1] <= 0x8F:
            continue                     # jcc rel32 inside the function: keep
        # any 4-byte little-endian word in the instruction that lands in the image
        for k in range(1, ins.size - 3):
            w = struct.unpack_from("<I", b, k)[0]
            if lo <= w < hi:
                out[o + k:o + k + 4] = b"\0\0\0\0"
    return bytes(out), calls


def name_functions(funcs, elf, ref):
    """{addr: name} for functions in `funcs` (of `elf`) that match one in `ref`.

    Lengths are the reference's: the finder's extents and the linker's sizes
    disagree by padding and cold tails, so a game function is compared over
    the reference function's length, found through a key on its first bytes."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    HEAD = 32

    def masked(e, a, n):
        lo, hi = e.image_base, e.image_base + e.image_size
        try:
            return _masked(md, e.read_va(a, n), a, lo, hi)
        except ValueError:
            return b"", []

    rfun = {a: (s, n) for a, (s, n) in ref.functions().items() if s >= 8}
    heads = defaultdict(list)
    for a, (sz, _n) in rfun.items():
        heads[masked(ref, a, min(sz, HEAD))[0]].append(a)

    full_ref = {}

    def ref_body(r):
        if r not in full_ref:
            full_ref[r] = masked(ref, r, rfun[r][0])
        return full_ref[r]

    def match(g, cands):
        hit = []
        for r in cands:
            body, calls = ref_body(r)
            gb, gc = masked(elf, g, len(body))
            if gb == body:
                hit.append((r, gc, calls))
        return hit[0] if len(hit) == 1 else None

    pairs, gcalls, rcalls = {}, {}, {}
    taken = set()
    for g, sz in funcs.items():
        cands = heads.get(masked(elf, g, min(sz, HEAD))[0])
        if not cands:
            continue
        m = match(g, cands)
        if m and m[0] not in taken:
            pairs[g] = m[0]
            taken.add(m[0])
            gcalls[g], rcalls[m[0]] = m[1], m[2]
    exact = len(pairs)

    # Built by another compiler, a function's bytes differ but the strings it
    # prints do not, and a library's functions print their own names
    # ("amDongleInit: chip exists."). A string only one function on each side
    # uses pairs those two, if every such string agrees on the pair.
    def strings_used(e, fs):
        ro = e.section(".rodata")
        lo, hi = ro.addr, ro.addr + ro.size
        users = defaultdict(set)
        for a, sz in fs.items():
            try:
                code = e.read_va(a, sz)
            except ValueError:
                continue
            for ins in md.disasm(code, a):
                b = ins.bytes
                for k in range(1, ins.size - 3):
                    w = struct.unpack_from("<I", b, k)[0]
                    if lo <= w < hi:
                        raw = e.read_va(w, 160).split(b"\0", 1)[0]
                        if len(raw) >= 6 and all(32 <= c < 127 or c in (9, 10) for c in raw):
                            users[raw].add(a)
        return users

    gu = strings_used(elf, funcs)
    ru = strings_used(ref, {a: s for a, (s, _n) in rfun.items()})
    votes = defaultdict(set)
    for text, g in gu.items():
        r = ru.get(text)
        if r and len(r) == 1 and len(g) == 1:
            votes[next(iter(g))].add(next(iter(r)))
    back = defaultdict(set)
    for g, rs_ in votes.items():
        for r in rs_:
            back[r].add(g)
    by_strings = 0
    for g, rs_ in votes.items():
        r = next(iter(rs_))
        if len(rs_) == 1 and len(back[r]) == 1 and g not in pairs and r not in taken:
            pairs[g] = r
            taken.add(r)
            by_strings += 1

    # Matched bodies call matched callees in the same order: pair them up too.
    work = list(pairs.items())
    while work:
        g, r = work.pop()
        gc, rc = gcalls.get(g, []), rcalls.get(r, [])
        if len(gc) != len(rc):
            continue
        for (go, gt), (ro, rt) in zip(gc, rc):
            if go != ro or gt not in funcs or gt in pairs or rt not in rfun or rt in taken:
                continue
            pairs[gt] = rt
            taken.add(rt)
            b, c = masked(elf, gt, rfun[rt][0])
            gcalls[gt], rcalls[rt] = c, ref_body(rt)[1]
            work.append((gt, rt))

    names = {g: rfun[r][1] for g, r in pairs.items() if rfun[r][1]}
    print("[*] named %d functions (%d by identical body, %d by the strings they use, "
          "%d through calls)" % (len(names), exact, by_strings,
                                 len(pairs) - exact - by_strings), file=sys.stderr)
    return names


def write_bounds(path, funcs, names):
    with open(path, "w") as f:
        f.write("# function bounds: 0xADDR size [name]  (tools bounds)\n")
        for a in sorted(funcs):
            n = names.get(a, "")
            f.write("0x%08X %d%s\n" % (a, funcs[a], " " + n if n else ""))


def read_bounds(path):
    """{addr: (size, name)} from a bounds file."""
    out = {}
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if len(line) >= 2:
                out[int(line[0], 16)] = (int(line[1]), line[2] if len(line) > 2 else "")
    return out


def read_names(path):
    """{addr: name} from a `0xADDR name` table."""
    out = {}
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if len(line) >= 2:
                out[int(line[0], 16)] = line[1]
    return out


def main(elf_path, out_path, ref_path=None, names_path=None):
    elf = Elf32(elf_path)
    funcs = find_functions(elf, elf.plt_map())
    names = name_functions(funcs, elf, Elf32(ref_path)) if ref_path else {}
    if names_path:
        table = read_names(names_path)
        lost = [n for a, n in table.items() if a not in funcs]
        if lost:
            raise SystemExit("%s names functions the finder did not find: %s"
                             % (names_path, ", ".join(lost)))
        names.update(table)
    write_bounds(out_path, funcs, names)
    print("wrote %d functions (%d named) to %s" % (len(funcs), len(names), out_path))
