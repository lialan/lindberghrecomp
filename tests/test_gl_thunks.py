"""Compile and execute generated Windows-style GL ABI thunks on the host."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GlThunkAbi(unittest.TestCase):
    def test_windows_headers_marshal_guest_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "imports.h").write_text('X(probe, "glProbeEXT")\nX(legacy, "glLegacy")\n')
            (d / "api.h").write_text('''
#define GLAPIENTRY
#define WINGDIAPI extern
typedef float GLfloat;
typedef double GLdouble;
WINGDIAPI void GLAPIENTRY glLegacy(GLfloat gain);
typedef void (GLAPIENTRY * PFNGLPROBEEXTPROC)(GLfloat gain, GLdouble wide, const GLfloat *values);
#define glProbeEXT probe_impl
void probe_impl(GLfloat gain, GLdouble wide, const GLfloat *values);
''')
            # A small guest window and the real generated C call conventions.
            (d / "gl_posix.h").write_text('''
#include <stdint.h>
#include <string.h>
#include "api.h"
typedef struct { uint32_t eax, args[8]; } CPU;
typedef void (*HleHandler)(CPU *);
static unsigned char memory[128];
#define A32(n) (c->args[n])
#define GP(n) (memory + (n))
static float gl_arg_f(CPU *c, unsigned n) { float f; memcpy(&f, &c->args[n], 4); return f; }
static double gl_arg_d(CPU *c, unsigned n) { double f; memcpy(&f, &c->args[n], 8); return f; }
static void gl_trace(const char *n) { (void)n; }
static void gl_check(const char *n) { (void)n; }
static void gl_bind_named(const char *n, HleHandler f) { (void)n; (void)f; }
static void gl_arg_words(const char *n, unsigned w) { (void)n; (void)w; }
static void hle_bind(const char *n, HleHandler f) { (void)n; (void)f; }
''')
            (d / "arbprog.h").write_text("")
            result = subprocess.run(
                [os.sys.executable, str(ROOT / "tools/gen_gl_thunks.py"), str(d / "imports.h"), str(d / "api.h")],
                text=True, capture_output=True, check=True,
            )
            self.assertIn("2 thunks", result.stderr)
            (d / "generated.c").write_text(result.stdout)
            (d / "test.c").write_text('''
#include <assert.h>
#include "generated.c"
static int hits;
void glLegacy(float f) { assert(f == 0.75f); hits++; }
void probe_impl(float f, double wide, const float *p) {
    assert(f == 0.75f && wide == 1.234567890123 && p == (float *)(memory + 32) && *p == -3.5f);
    hits++;
}
int main(void) {
    CPU c = {0}; float f = 0.75f, value = -3.5f; double wide = 1.234567890123;
    memcpy(&c.args[0], &f, 4); memcpy(&c.args[1], &wide, 8); c.args[3] = 32;
    memcpy(memory + 32, &value, 4);
    gl_register_generated(); th_glLegacy(&c); th_glProbeEXT(&c);
    assert(hits == 2); return 0;
}
''')
            subprocess.run([os.environ.get("HOST_CC", "cc"), "-std=c11", "-Wall", "-Werror", "-I", str(d), str(d / "test.c"), "-o", str(d / "test")], check=True)
            subprocess.run([str(d / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
