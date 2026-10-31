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

A goto within the function takes the flags its label needs, found by
iterating to a fixed point over the function's labels (a goto to a label
elsewhere, and every other jump, still needs all). A cmp or test whose flags
nothing reads past the conditional jump right after it is fused with that
jump: the comparison itself, as C, instead of six flags stored and read
back (fuse_compare).
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


_label = re.compile(r"^(L_[0-9A-Fa-f]+):\s*$")
_goto = re.compile(r"^\s*(?:if \((.*)\) )?goto (L_[0-9A-Fa-f]+);\s*(/\*.*\*/)?\s*$")
_cmp = re.compile(r"^(\s*)(flags_sub|flags_logicz)\(c, (.*)\);\s*(/\*.*\*/)?\s*$")

# A condition as the lifter writes it (spaces removed) over cmp a, b: the C
# comparison; over test x: the same on x alone. {u}/{s}: the unsigned and
# signed type of the operand size.
SUB_COND = {
    "c->zf": "({u})({a}) == ({u})({b})", "!c->zf": "({u})({a}) != ({u})({b})",
    "c->cf": "({u})({a}) < ({u})({b})", "!c->cf": "({u})({a}) >= ({u})({b})",
    "(c->sf!=c->of)": "({s})({a}) < ({s})({b})", "(c->sf==c->of)": "({s})({a}) >= ({s})({b})",
    "(c->zf||(c->sf!=c->of))": "({s})({a}) <= ({s})({b})", "(!c->zf&&(c->sf==c->of))": "({s})({a}) > ({s})({b})",
    "(!c->cf&&!c->zf)": "({u})({a}) > ({u})({b})", "(c->cf||c->zf)": "({u})({a}) <= ({u})({b})",
    "c->sf": "({s})(({u})({a}) - ({u})({b})) < 0", "!c->sf": "({s})(({u})({a}) - ({u})({b})) >= 0",
}
TEST_COND = {
    "c->zf": "({u})({x}) == 0", "!c->zf": "({u})({x}) != 0",
    "c->sf": "({s})({x}) < 0", "!c->sf": "({s})({x}) >= 0",
    "(c->sf!=c->of)": "({s})({x}) < 0", "(c->sf==c->of)": "({s})({x}) >= 0",
    "(c->zf||(c->sf!=c->of))": "({s})({x}) <= 0", "(!c->zf&&(c->sf==c->of))": "({s})({x}) > 0",
    "(!c->cf&&!c->zf)": "({u})({x}) != 0", "(c->cf||c->zf)": "({u})({x}) == 0",
}
TYPES = {"1": ("uint8_t", "int8_t"), "2": ("uint16_t", "int16_t"), "4": ("uint32_t", "int32_t")}


def _split_args(text):
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip()); cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return out


def fuse_compare(cmp_line, cond):
    """The C condition for cmp_line's flags tested by cond, or None."""
    m = _cmp.match(cmp_line)
    if not m:
        return None
    args = _split_args(m.group(3))
    key = cond.replace(" ", "")
    if m.group(2) == "flags_sub" and len(args) == 3 and args[2] in TYPES and key in SUB_COND:
        u, sg = TYPES[args[2]]
        return SUB_COND[key].format(u=u, s=sg, a=args[0], b=args[1])
    if m.group(2) == "flags_logicz" and len(args) == 2 and args[1] in TYPES and key in TEST_COND:
        u, sg = TYPES[args[1]]
        return TEST_COND[key].format(u=u, s=sg, x=args[0])
    return None


def _pass(lines, live_in, labels, rewrite):
    """One backward pass. Returns the labels' liveness found (and, rewriting,
    the new lines and the count changed)."""
    live = ALL
    out = list(lines)
    found = {}
    dropped = 0
    fuse_next = None            # (line index of the goto, its condition, flags live past it)
    for i in range(len(lines) - 1, -1, -1):
        line = lines[i]
        s = line.strip()
        if not s or s.startswith("/*"):
            continue
        m = _label.match(s)
        if m:
            found[m.group(1)] = live    # liveness flows up through it unchanged
            fuse_next = None
            continue
        if s.startswith(("case ", "default:")):
            continue
        g = _goto.match(line)
        if g:
            cond, target = g.group(1), g.group(2)
            there = live_in.get(target, NONE) if target in labels else ALL
            if cond is None:
                live = there
                fuse_next = None
            else:
                after = live | there
                fuse_next = (i, cond, after)
                live = after | frozenset(_read.findall(cond))
            continue
        if s in ("{", "}") or s.endswith("{") or s.startswith("}"):
            live = ALL
            fuse_next = None
            continue
        eff = _line_effect(line)
        if eff is None:
            live = ALL
            fuse_next = None
            continue
        if eff == "abi":
            live = NONE
            fuse_next = None
            continue
        reads, writes, rw = eff
        if rewrite and fuse_next and rw in ("flags_sub", "flags_logicz") and not (fuse_next[2] & ALL):
            j, cond, after = fuse_next
            expr = fuse_compare(line, cond)
            if expr is not None:
                gm = _goto.match(lines[j])
                out[j] = re.sub(r"if \(.*\) goto", "if (%s) goto" % expr.replace("\\", "\\\\"), lines[j], count=1)
                ind, comment = _cmp.match(line).group(1), _cmp.match(line).group(4) or ""
                out[i] = ind + "/* fused with the jump below */" + (" " + comment if comment else "")
                dropped += 1
                live = after             # its flags are gone, and nothing before needs them for this jump
                fuse_next = None
                continue
        fuse_next = None
        if rewrite and rw and not (live & writes):
            out[i] = line.replace(rw + "(c, ", REWRITE[rw] + "(", 1)
            dropped += 1
            continue                    # its writes were dead; live unchanged
        live = (live - writes) | reads
    return found, out, dropped


def _process_function(lines):
    labels = {m.group(1) for m in (_label.match(l.strip()) for l in lines) if m}
    live_in = {}
    for _ in range(64):         # to a fixed point: liveness only grows
        found, _, _ = _pass(lines, live_in, labels, False)
        if found == live_in:
            break
        live_in = found
    else:
        live_in = {l: ALL for l in labels}       # did not settle: assume the worst
    _, out, dropped = _pass(lines, live_in, labels, True)
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
