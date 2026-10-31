#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "glsl_compat.h"

static int ident(unsigned char c) { return isalnum(c) || c == '_'; }
static const char *space(const char *p) { while (isspace((unsigned char)*p)) p++; return p; }
static int equal(const char *p, size_t n, const char *s) { return strlen(s) == n && !memcmp(p, s, n); }
static void copy(char **out, const char *p, size_t n) { memcpy(*out, p, n); *out += n; }
static const char *call_end(const char *p)
{
    int depth = 0;
    do { if (*p == '(') depth++; if (*p == ')') depth--; p++; } while (*p && depth);
    return depth ? NULL : p;
}
static const char *opaque(const char *p)
{
    if (*p == '#' || (p[0] == '/' && p[1] == '/')) {
        const char *end = strchr(p, '\n'); return end ? end : p + strlen(p);
    }
    if (p[0] == '/' && p[1] == '*') {
        const char *end = strstr(p + 2, "*/"); return end ? end + 2 : p + strlen(p);
    }
    return p;
}

char *glsl_compat(const char *source)
{
    size_t len = strlen(source);
    if (len > (SIZE_MAX - 64) / 4) return NULL;
    char *half = malloc(2 * len + 1), *result = malloc(4 * len + 64);
    if (!half || !result) { free(half); free(result); return NULL; }
    char *w = half;
    for (const char *p = source; *p;) {
        const char *end = opaque(p);
        if (end != p) { copy(&w, p, end - p); p = end; continue; }
        if (isalpha((unsigned char)*p) || *p == '_') {
            end = p + 1; while (ident((unsigned char)*end)) end++;
            size_t n = end - p;
            const char *replace = equal(p, n, "half") ? "float" :
                                  equal(p, n, "half2") ? "vec2" :
                                  equal(p, n, "half3") ? "vec3" :
                                  equal(p, n, "half4") ? "vec4" : NULL;
            if (replace) copy(&w, replace, strlen(replace)); else copy(&w, p, n);
            p = end;
        } else *w++ = *p++;
    }
    *w = 0; w = result;
    int scope = 0, parens = 0;
    for (const char *p = half; *p;) {
        const char *end = opaque(p);
        if (end != p) { copy(&w, p, end - p); p = end; continue; }
        if (!isalpha((unsigned char)*p) && *p != '_') {
            if (*p == '{') scope++; else if (*p == '}') scope--;
            else if (*p == '(') parens++; else if (*p == ')') parens--;
            *w++ = *p++; continue;
        }
        end = p + 1; while (ident((unsigned char)*end)) end++;
        size_t n = end - p;
        if (!scope && !parens && n > 7 && !memcmp(p, "sampler", 7)) {
            const char *prev_end = p;
            while (prev_end > half && isspace((unsigned char)prev_end[-1])) prev_end--;
            const char *prev = prev_end;
            while (prev > half && ident((unsigned char)prev[-1])) prev--;
            if (!equal(prev, prev_end - prev, "uniform")) copy(&w, "uniform ", 8);
        }
        /* Cg/NVIDIA accepts a numeric tuple as dot's vector operand, and
         * truncates the other vector to that width. Standard GLSL treats
         * the tuple as a scalar comma expression instead. */
        if (equal(p, n, "dot") && *space(end) == '(') {
            const char *lhs = space(space(end) + 1), *comma = lhs;
            while (ident((unsigned char)*comma) || *comma == '.') comma++;
            const char *sep = space(comma);
            if (*sep == ',' && *space(sep + 1) == '(') {
                const char *tuple = space(sep + 1), *at = tuple + 1;
                int lanes = 0;
                while (lanes < 4) {
                    char *e; strtod(space(at), &e);
                    if (e == space(at)) break;
                    lanes++; at = space(e);
                    if (*at != ',') break;
                    at++;
                }
                if (lanes >= 2 && *at == ')' && *space(at + 1) == ')') {
                    char type[] = "vec2"; type[3] = (char)('0' + lanes);
                    copy(&w, "dot(", 4); copy(&w, type, 4); *w++ = '(';
                    copy(&w, lhs, comma - lhs); copy(&w, "),", 2);
                    copy(&w, type, 4); *w++ = '(';
                    copy(&w, tuple + 1, at - tuple - 1); copy(&w, "))", 2);
                    p = space(at + 1) + 1; continue;
                }
            }
        }
        /* A scalar dot result assigned to a vector swizzle is broadcast. */
        if (*end == '.') {
            const char *sw = end + 1, *se = sw;
            while (*se && strchr("rgbaxyzw", *se)) se++;
            const char *eq = space(se);
            const char *rhs = *eq == '=' && eq[1] != '=' ? space(eq + 1) : eq;
            if (se - sw >= 2 && se - sw <= 4 && !strncmp(rhs, "dot", 3) && *space(rhs + 3) == '(') {
                const char *ce = call_end(space(rhs + 3));
                if (ce && *space(ce) == ';') {
                    char type[] = "vec2"; type[3] = (char)('0' + (se - sw));
                    copy(&w, p, rhs - p); copy(&w, type, 4); *w++ = '(';
                    copy(&w, rhs, ce - rhs); *w++ = ')'; p = ce; continue;
                }
            }
        }
        int vector = n == 4 && !memcmp(p, "vec", 3) && p[3] >= '2' && p[3] <= '4';
        int scalar = equal(p, n, "float");
        int matrix = n == 4 && !memcmp(p, "mat", 3) && p[3] >= '2' && p[3] <= '4';
        const char *name = space(end), *eq = name;
        if (isalpha((unsigned char)*eq) || *eq == '_') {
            while (ident((unsigned char)*eq)) eq++;
            eq = space(eq);
            if ((vector || scalar || matrix) && *eq == '=' && eq[1] != '=') {
                const char *rhs = space(eq + 1);
                if (matrix && *rhs == '{') {
                    const char *me = rhs; int depth = 0;
                    do { if (*me == '{') depth++; if (*me == '}') depth--; me++; } while (*me && depth);
                    if (!depth && *space(me) == ';') {
                        copy(&w, p, rhs - p); copy(&w, p, n); *w++ = '(';
                        for (const char *v = rhs + 1; v < me - 1; v++) *w++ = (*v == '{' || *v == '}') ? ' ' : *v;
                        *w++ = ')'; p = me; continue;
                    }
                } else if (vector) {
                    char *number_end;
                    strtod(rhs, &number_end);
                    if (number_end != rhs) {
                        if (*number_end == 'f' || *number_end == 'F') number_end++;
                        if (*space(number_end) == ';') {
                            copy(&w, p, rhs - p); copy(&w, p, n); *w++ = '(';
                            copy(&w, rhs, number_end - rhs); *w++ = ')';
                            p = number_end; continue;
                        }
                    }
                } else if (!strncmp(rhs, "shadow2D", 8) && *space(rhs + 8) == '(') {
                    const char *ce = call_end(space(rhs + 8));
                    if (ce && *space(ce) == ';') {
                        copy(&w, p, ce - p); copy(&w, ".r", 2);
                        p = ce; continue;
                    }
                }
            }
        }
        copy(&w, p, n); p = end;
    }
    *w = 0; free(half); return result;
}
