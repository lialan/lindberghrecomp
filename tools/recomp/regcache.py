"""Keep a lifted function's x86 registers in C locals.

The lifter writes every register as a field of the CPU struct (c->eax). A
guest memory store is a store through a pointer the compiler cannot tell
from c, so after each one it reloads every register it reads next from the
struct - and lifted code stores to guest memory every few instructions. In a
function's own body nothing else can see the registers, so they can be
locals, which the compiler keeps in host registers.

The struct is the truth only where code outside the function looks at it:
calls and returns. So each local is written back before a call (DISPATCH_TO,
dispatch, hle_call, ...) and read again after it, and written back before a
return. push32/pop32 move the local esp. A helper that takes c and touches
only flags, the FPU or SSE state is left alone (SAFE); any other call that
takes c counts as a call, which is always correct, only slower.

The SSE registers (c->xmm[n]) the same way: the float code of these games
moves values through them every few instructions, each a load or store of
the struct. No SAFE helper reads or writes c->xmm (fxsave and fxrstor do,
and count as calls).
"""
import re

GPRS = ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp")

# Take c, touch no general register (cpu.h).
SAFE = {
    "flags_logicz", "flags_sub", "flags_add", "flags_adc", "flags_sbb", "flags_incs",
    "flags_decs", "op_shl", "op_shr", "op_sar", "op_rol", "op_ror", "op_shld", "op_shrd",
    "sse_compare", "fst", "fpop", "fpush", "fcompare", "fprem_op", "do_sahf",
    "bit_string_op",
}

_func = re.compile(r"^void (L_[0-9A-F]{8})\(CPU \*c\)\n\{\n(.*?)\n\}$", re.S | re.M)
_call = re.compile(r"\b([A-Za-z_][A-Za-z_0-9]*)\(c(?=[,)])")
_reg = re.compile(r"\bc->(eax|ebx|ecx|edx|esi|edi|ebp|esp)\b")
_xmm = re.compile(r"\bc->xmm\[([0-7])\]")
_ret = re.compile(r"\breturn;")


def _close(s, i):
    """Index past the ')' matching the '(' at s[i]."""
    depth = 0
    while i < len(s):
        ch = s[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced")


def _body(body):
    used = set(_reg.findall(body))
    if "push32(c," in body or "pop32(c)" in body:
        used.add("esp")
    xmms = sorted(set(_xmm.findall(body)))
    if not used and not xmms:
        return body, 0
    regs = [r for r in GPRS if r in used]
    # "c@" until the body's own c->reg are renamed, then "c->"
    spill = " ".join(["c@%s = R_%s;" % (r, r) for r in regs] + ["c@xmm[%s] = X_xmm%s;" % (x, x) for x in xmms])
    fill = " ".join(["R_%s = c@%s;" % (r, r) for r in regs] + ["X_xmm%s = c@xmm[%s];" % (x, x) for x in xmms])

    out, i, n = [], 0, 0
    while True:
        m = _call.search(body, i)
        if not m:
            out.append(body[i:])
            break
        name = m.group(1)
        end = _close(body, m.start(1) + len(name))
        if name == "push32":
            out.append(body[i:m.start()])
            out.append("PUSHL(" + body[m.end() + 1:end].lstrip())
            i = end
            continue
        if name == "pop32":
            out.append(body[i:m.start()])
            out.append("POPL()")
            i = end
            continue
        if name in SAFE:
            out.append(body[i:end])
            i = end
            continue
        # a call: must be a statement of its own
        if body[end:end + 1] != ";":
            raise ValueError("call to %s inside an expression" % name)
        out.append(body[i:m.start()])
        out.append("{ %s %s; %s }" % (spill, body[m.start():end], fill))
        i = end + 1
        n += 1
    body = "".join(out)
    body = _ret.sub("{ %s return; }" % spill, body)
    body = _reg.sub(lambda m: "R_" + m.group(1), body)
    body = _xmm.sub(lambda m: "X_xmm" + m.group(1), body)
    decl = ""
    if regs:
        decl += "    uint32_t " + ", ".join("R_%s = c->%s" % (r, r) for r in regs) + ";\n"
    if xmms:
        decl += "    XMM " + ", ".join("X_xmm%s = c->xmm[%s]" % (x, x) for x in xmms) + ";\n"
    return (decl + body + "\n    " + spill).replace("c@", "c->"), n


def cache_registers(text):
    """Rewrite every lifted function in text; returns (text, functions)."""
    count = 0

    def one(m):
        nonlocal count
        body, _ = _body(m.group(2))
        count += 1
        return "void %s(CPU *c)\n{\n%s\n}" % (m.group(1), body)

    return _func.sub(one, text), count


PRELUDE = (
    "/* regcache.py: the registers are locals; push and pop move the local esp */\n"
    "#define PUSHL(v) do { uint32_t _pv = (uint32_t)(v); R_esp -= 4; wr32(R_esp, _pv); } while (0)\n"
    "#define POPL() (R_esp += 4, rd32(R_esp - 4))\n"
)
