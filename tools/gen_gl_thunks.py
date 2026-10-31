"""
gen_gl_thunks.py - typed OpenGL thunks for a 64-bit host.

On a 32-bit x86 host a GL import can be forwarded as raw stack dwords to an
__stdcall pointer. On arm64 (and x86-64) that is wrong twice over: floats and
doubles travel in FP registers, and a guest pointer is a 32-bit offset into the
guest window rather than an address. So every GL import the game makes gets a
thunk with the real prototype: each argument is read from the guest stack at
its own width (a GLdouble is two slots), pointers are translated, and the call
is a normal C call the host compiler gets right.

Prototypes come from the host's own GL headers (the `...ProcPtr` typedefs in
the macOS SDK, or `PFN...PROC` in Khronos glext.h). The guest's ARB/EXT names
are routed to core entry points where one exists, because GLES - the Quest -
has only those.

    python3 gen_gl_thunks.py recomp_imports.h gl.h glext.h > recomp_gl_thunks.c

A library whose headers declare more than it defines (gl4es, for the Quest)
also takes the names it does define, one per line:

    nm -g --defined-only libGL.a | awk '{print $3}' > gl.exports
    python3 gen_gl_thunks.py --exports gl.exports recomp_imports.h gl.h glext.h
"""
import re
import sys

# Entry points implemented by hand in hle_gl_posix.c; the generator skips them.
MANUAL = {
    "glGetString", "glMapBufferARB", "glUnmapBufferARB", "glBindBufferARB",
    "glShaderSourceARB", "glCreateProgramObjectARB", "glCreateShaderObjectARB",
    "glAttachObjectARB", "glUseProgramObjectARB", "glDeleteObjectARB",
    "glGetObjectParameterivARB", "glGetInfoLogARB", "glLinkProgramARB",
    "glCompileShaderARB", "glGetUniformLocationARB", "glGetAttribLocationARB",
    "gluErrorString", "gluOrtho2D", "gluPerspective", "gluLookAt",
    "glOrtho", "glFrustum", "glProgramStringARB", "glMultiDrawElements", "glMultiDrawElementsEXT",
    "glBindBuffer", "glMapBuffer", "glUnmapBuffer",
    "glBindProgramARB", "glDeleteProgramsARB", "glProgramEnvParameter4fvARB",
    "glProgramLocalParameter4fvARB", "glProgramLocalParameter4fARB",
    "glEnable", "glDisable", "glIsEnabled", "glPushAttrib", "glPopAttrib",
    "glProgramParameters4fvNV",
}

# Calls that draw: the assembly-program state is turned into a GLSL program
# just before them (arbprog.c).
DRAWS = {"glDrawArrays", "glDrawElements", "glDrawRangeElements", "glBegin"}

# ARB_vertex/fragment_program has no core equivalent; glGetProgramivARB is a
# different function from core glGetProgramiv despite the name.
KEEP_ARB = re.compile(r"Program(String|Env|Local|sARB|ivARB)|BindProgramARB|GenProgramsARB|DeleteProgramsARB")

# Arguments that are a client pointer only while no buffer is bound to the
# named target - with one bound they are byte offsets into it.
OFFSET_ARGS = {
    "glVertexPointer": (3, "GL_ARRAY_BUFFER"),
    "glNormalPointer": (2, "GL_ARRAY_BUFFER"),
    "glColorPointer": (3, "GL_ARRAY_BUFFER"),
    "glTexCoordPointer": (3, "GL_ARRAY_BUFFER"),
    "glSecondaryColorPointer": (3, "GL_ARRAY_BUFFER"),
    "glVertexAttribPointer": (5, "GL_ARRAY_BUFFER"),
    "glDrawElements": (3, "GL_ELEMENT_ARRAY_BUFFER"),
    "glDrawRangeElements": (5, "GL_ELEMENT_ARRAY_BUFFER"),
    "glTexImage2D": (8, "GL_PIXEL_UNPACK_BUFFER"),
    "glTexSubImage2D": (8, "GL_PIXEL_UNPACK_BUFFER"),
    "glCompressedTexImage2D": (7, "GL_PIXEL_UNPACK_BUFFER"),
    "glCompressedTexSubImage2D": (8, "GL_PIXEL_UNPACK_BUFFER"),
    "glReadPixels": (6, "GL_PIXEL_PACK_BUFFER"),
    "glGetTexImage": (4, "GL_PIXEL_PACK_BUFFER"),
}

PROC = re.compile(r"typedef\s+(.+?)\s*\(\s*(?:APIENTRYP|\*)\s*(?:PFN(\w+)PROC|(\w+)ProcPtr)\s*\)\s*\((.*?)\)\s*;", re.S)
EXTERN = re.compile(r"(?:extern|GLAPI)\s+(.+?)\s*(?:(?:GL)?APIENTRY\s+)?\b(gl\w+)\s*\((.*?)\)", re.S)


EXPORTED = set()      # names with a real prototype: the library exports them


def visible(headers):
    """Names the headers actually declare once preprocessed. glext.h guards
    most of its prototypes; a name inside an inactive #if has no symbol."""
    import os, subprocess, tempfile
    src = "#define GL_SILENCE_DEPRECATION 1\n#define GL_GLEXT_PROTOTYPES 1\n"
    src += "".join(f'#include "{os.path.abspath(h)}"\n' for h in headers)
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(src)
    try:
        out = subprocess.run([os.environ.get("CC", "cc"), "-E", "-P", f.name],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    finally:
        os.unlink(f.name)
    return set(re.findall(r"\b(gl[A-Z]\w*)\s*\(", out))


def load(headers):
    sig = {}
    for h in headers:
        text = open(h, encoding="latin-1").read()
        for m in PROC.finditer(text):
            ret, upper, name, args = m.groups()
            if name:
                sig.setdefault(name, (ret.strip(), args))
        for m in EXTERN.finditer(text):
            ret, name, args = m.groups()
            sig.setdefault(name, (ret.strip(), args))
            EXPORTED.add(name)
    return sig


def split_args(args):
    args = " ".join(args.split())
    if args in ("", "void"):
        return []
    out = []
    for a in args.split(","):
        a = a.strip()
        m = re.match(r"(.*?)(\w+)\s*(\[\d*\])?$", a)
        typ, nm = m.group(1).strip(), m.group(2)
        if m.group(3):
            typ += " *"
        if not typ:          # unnamed parameter: the whole thing is the type
            typ, nm = a, None
        out.append(typ)
    return out


def core_name(name, sig):
    if KEEP_ARB.search(name):
        return name
    for suf in ("ARB", "EXT"):
        if name.endswith(suf) and name[: -len(suf)] in sig:
            return name[: -len(suf)]
    return name


WORDS = {}      # name -> dwords of arguments on the guest stack


def emit(name, target, ret, types):
    lines, call, slot = [], [], 0
    off = OFFSET_ARGS.get(target)
    for i, t in enumerate(types):
        base = t.replace("const", "").strip()
        if "*" in t:
            if off and off[0] == i:
                call.append(f"({t})gl_ptr_or_offset({off[1]}, A32({slot}))")
            else:
                call.append(f"({t})GP(A32({slot}))")
            slot += 1
        elif base in ("GLdouble", "GLclampd"):
            call.append(f"gl_arg_d(c, {slot})")
            slot += 2
        elif base in ("GLfloat", "GLclampf"):
            call.append(f"gl_arg_f(c, {slot})")
            slot += 1
        elif base in ("GLint64", "GLuint64", "GLint64EXT", "GLuint64EXT"):
            call.append(f"({base})((uint64_t)A32({slot}) | ((uint64_t)A32({slot + 1}) << 32))")
            slot += 2
        elif base == "GLhandleARB":
            call.append(f"(GLhandleARB)(uintptr_t)A32({slot})")
            slot += 1
        else:
            call.append(f"({base})A32({slot})")
            slot += 1
    WORDS[name] = slot
    expr = f"{target}({', '.join(call)})"
    r = ret.replace("const", "").strip()
    chk = f"    gl_check(\"{name}\");\n"
    if r == "void":
        body = f"    {expr};\n" + chk
    elif r in ("GLfloat", "GLdouble"):
        body = f"    RETF({expr});\n" + chk
    elif "*" in r or r == "GLhandleARB":
        return None          # a returned pointer needs a hand-written thunk
    else:
        body = f"    c->eax = (uint32_t){expr};\n" + chk
    hook = "    arb_before_draw();\n" if target in DRAWS else ""
    return f"static void th_{name}(CPU *c)\n{{\n    gl_trace(\"{name}\");\n{hook}{body}}}\n"


def main():
    argv = sys.argv[1:]
    defined = None
    if argv[:1] == ["--exports"]:
        defined = set(open(argv[1]).read().split())
        argv = argv[2:]
    imports, headers = argv[0], argv[1:]
    names = re.findall(r'X\(\s*\w+\s*,\s*"(glu?[A-Z]\w*)"\s*\)', open(imports).read())
    sig = load(headers)
    seen = visible(headers)
    if seen is not None:
        EXPORTED.intersection_update(seen)
    if defined is not None:
        EXPORTED.intersection_update(defined)
        for n in list(sig):
            if n not in defined:
                del sig[n]
    out = ["/* GENERATED by tools/gen_gl_thunks.py - do not edit. */",
           '#include "gl_posix.h"', '#include "arbprog.h"', ""]
    bound, missing = [], []
    for n in sorted(set(names)):
        if n in MANUAL:
            continue
        target = core_name(n, sig)
        if target not in sig:
            missing.append(n)
            continue
        ret, args = sig[target]
        code = emit(n, target, ret, split_args(args))
        if code is None:
            missing.append(n)
            continue
        out.append(code)
        bound.append(n)
    # Everything else the library exports, by name: what a GLX game fetches
    # with glXGetProcAddressARB instead of importing (gl_token_for).
    extra = []
    for n in sorted(sig):
        if n in MANUAL or n in set(names) or not n.startswith("gl") or n.startswith("glu"):
            continue
        target = core_name(n, sig)
        if target not in EXPORTED:
            continue
        ret, args = sig[target]
        try:
            code = emit(n, target, ret, split_args(args))
        except (AttributeError, ValueError):
            code = None           # a prototype this parser does not read (function pointers)
        if code is None:
            continue
        out.append(code)
        extra.append(n)
    out.append("void gl_register_generated(void)\n{")
    for n in bound:
        out.append(f'    hle_bind("{n}", th_{n}); gl_bind_named("{n}", th_{n}); gl_arg_words("{n}", {WORDS[n]});')
    for n in extra:
        out.append(f'    gl_bind_named("{n}", th_{n}); gl_arg_words("{n}", {WORDS[n]});')
    out.append("}")
    if missing:
        out.append("\n/* not generated (no prototype or returns a pointer): " + ", ".join(missing) + " */")
    print("\n".join(out))
    print(f"[gen_gl] {len(bound)} thunks, {len(extra)} by name only, {len(missing)} left: {missing}", file=sys.stderr)


main()
