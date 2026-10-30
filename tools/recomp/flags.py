#!/usr/bin/env python3
"""
flags.py - drop the flags nothing reads, from lifted C.

The lifter computes all six status flags for every add, sub, and, or, xor,
cmp and test - flags_add() and friends store CF, ZF, SF, OF, AF and PF and
work out parity - and nearly all of them are overwritten by the next such
instruction before anything looks. On the Quest that work was a large part
of a frame in hand-written loops.

This runs over a translation unit, a function at a time and backwards, and
tracks which flags may still be read. Where an instruction's flags are all
dead it becomes nf_add/nf_sub/nf_logicz (lindbergh_rt.h): the same
arithmetic, no flags. It is conservative wherever it cannot see: a label is
where flags may arrive from elsewhere, but liveness flows up through it
unchanged; a jump - goto, dispatch, a tail call - or a call into the
runtime it does not know needs every flag; a call or a return needs none
(the x86 calling conventions keep no flags across either); any explicit
read of c->cf and the rest keeps those flags; inc/dec pass CF through.
"""

import re

FLAGS = ("cf", "zf", "sf", "of", "af", "pf")
ALL = frozenset(FLAGS)
NONE = frozenset()

# Calls that take the CPU and leave the status flags alone.
NEUTRAL = {"push32", "pop32", "fst", "fpush", "fpop"}
# The three this rewrites; each writes all six flags and reads none.
REWRITE = {"flags_add": "nf_add", "flags_sub": "nf_sub", "flags_logicz": "nf_logicz"}

_read = re.compile(r"c->(cf|zf|sf|of|af|pf)\b")
_call = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\(c[,)]")
_barrier = re.compile(r"\b(goto|return|dispatch|dispatch_jmp|DISPATCH_TO|hle_call|RECOMP_TODO|longjmp)\b")


_mnem = re.compile(r"/\* [0-9A-F]{8}: ([a-z0-9]+)")


def _line_effect(line):
    """(reads, writes, rewritable_name) for one lifted line, None when the
    line is a barrier (every flag may be read past it), or "abi" for a call
    or a return, which by the calling convention neither take flags in nor
    hand any back."""
    m = _mnem.search(line)
    if m and (m.group(1) == "call" or m.group(1).startswith("ret")):
        return "abi"
    code = line.split("/*", 1)[0]
    if _barrier.search(code):
        return None
    calls = _call.findall(code)
    reads = set(_read.findall(code))
    writes = set()
    rw = None
    for name in calls:
        if name in NEUTRAL:
            continue
        if name in REWRITE:
            if rw is not None:
                return None             # two on one line: leave it alone
            rw = name
            writes |= ALL
            continue
        if name in ("flags_incs", "flags_decs"):
            writes |= ALL - {"cf"}
            continue
        return None                     # unknown: may read or write anything
    if reads and writes:
        return None
    return frozenset(reads), frozenset(writes), rw


def _process_function(lines):
    live = ALL
    out = list(lines)
    dropped = 0
    for i in range(len(lines) - 1, -1, -1):
        line = lines[i]
        s = line.strip()
        if not s or s.startswith("/*"):
            continue
        if re.match(r"^L_[0-9A-Fa-f]+:\s*$", s) or s.startswith(("case ", "default:")):
            continue                    # a label: liveness flows up unchanged
        if s in ("{", "}") or s.endswith("{") or s.startswith("}"):
            live = ALL
            continue
        eff = _line_effect(line)
        if eff is None:
            live = ALL
            continue
        if eff == "abi":
            live = NONE
            continue
        reads, writes, rw = eff
        if rw and not (live & writes):
            out[i] = line.replace(rw + "(c, ", REWRITE[rw] + "(", 1)
            dropped += 1
            continue                    # its writes were dead; live unchanged
        live = (live - writes) | reads
    return out, dropped


def strip_dead_flags(text):
    """Rewrite a lifted translation unit; returns (text, instructions changed)."""
    lines = text.split("\n")
    out, total, start = [], 0, None
    for i, line in enumerate(lines):
        if start is None:
            out.append(line)
            if re.match(r"^void L_[0-9A-F]+\(CPU \*c\)\s*$", line) and i + 1 < len(lines) and lines[i + 1] == "{":
                start = len(out)
            continue
        if line == "}":
            body, n = _process_function(out[start:])
            out[start:] = body
            total += n
            start = None
        out.append(line)
    return "\n".join(out), total
