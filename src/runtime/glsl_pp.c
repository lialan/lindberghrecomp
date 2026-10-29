/*
 * glsl_pp.c - a shader's #if/#ifdef blocks, decided before the driver sees it.
 *
 * Desktop GLSL follows C: an identifier in #if that is not a macro is 0.
 * GLSL ES does not, and the Quest's compiler rejects the line. The House of
 * the Dead 4's shaders (Sega's NN library) test their option macros both
 * ways - #ifdef X in one place, #if X in another, X defined only on some
 * paths - so no list of extra #defines fits every variant. Instead the
 * conditionals are evaluated here as a desktop compiler would, and only the
 * lines they keep are passed on. #define and #undef stay (the compiler still
 * expands the macros in the code); every directive that decided something
 * becomes an empty line, so the line numbers in the driver's errors are the
 * game's own.
 *
 * Object-like macros only - the game's shaders define no function-like ones,
 * and one in an #if expression evaluates as 0.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "glsl_pp.h"

#define MAX_MACROS 1024
#define MAX_DEPTH  64

typedef struct { char name[64]; char *value; } Macro;

typedef struct {
    Macro m[MAX_MACROS];
    int   n;
} Table;

static Macro *find(Table *t, const char *name, size_t len)
{
    for (int i = t->n - 1; i >= 0; i--)
        if (strlen(t->m[i].name) == len && !memcmp(t->m[i].name, name, len)) return &t->m[i];
    return NULL;
}

static void undef(Table *t, const char *name, size_t len)
{
    Macro *m = find(t, name, len);
    if (!m) return;
    free(m->value);
    *m = t->m[--t->n];
}

static void define(Table *t, const char *name, size_t len, const char *value)
{
    undef(t, name, len);
    if (t->n == MAX_MACROS || len >= sizeof t->m[0].name) return;
    Macro *m = &t->m[t->n++];
    memcpy(m->name, name, len);
    m->name[len] = 0;
    m->value = strdup(value);
}

/* ---- expressions: tokens, then recursive descent ---- */
enum { T_NUM, T_OP, T_END };
typedef struct { int kind; long v; char op[3]; } Tok;
typedef struct { Tok *t; int n, cap, at; } Toks;

static void push(Toks *ts, Tok k)
{
    if (ts->n == ts->cap) {
        ts->cap = ts->cap ? ts->cap * 2 : 64;
        ts->t = realloc(ts->t, (size_t)ts->cap * sizeof *ts->t);
    }
    ts->t[ts->n++] = k;
}

static int ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int ident_char(char c)  { return isalnum((unsigned char)c) || c == '_'; }

static void lex(Table *tab, const char *s, Toks *ts, int depth)
{
    while (*s) {
        if (isspace((unsigned char)*s)) { s++; continue; }
        if (isdigit((unsigned char)*s)) {
            char *end;
            long v = strtol(s, &end, 0);
            while (ident_char(*end) || *end == '.') end++;     /* 1u, 1.0 */
            push(ts, (Tok){ T_NUM, v, "" });
            s = end;
            continue;
        }
        if (ident_start(*s)) {
            const char *a = s;
            while (ident_char(*s)) s++;
            size_t len = (size_t)(s - a);
            if (len == 7 && !memcmp(a, "defined", 7)) {
                while (isspace((unsigned char)*s)) s++;
                int paren = *s == '(';
                if (paren) s++;
                while (isspace((unsigned char)*s)) s++;
                const char *b = s;
                while (ident_char(*s)) s++;
                push(ts, (Tok){ T_NUM, find(tab, b, (size_t)(s - b)) != NULL, "" });
                while (isspace((unsigned char)*s)) s++;
                if (paren && *s == ')') s++;
                continue;
            }
            Macro *m = find(tab, a, len);
            if (m && depth < MAX_DEPTH) lex(tab, m->value, ts, depth + 1);
            else push(ts, (Tok){ T_NUM, 0, "" });   /* not a macro: 0, as in C */
            continue;
        }
        static const char *two[] = { "||", "&&", "==", "!=", "<=", ">=", "<<", ">>" };
        Tok k = { T_OP, 0, "" };
        for (unsigned i = 0; i < sizeof two / sizeof *two; i++)
            if (s[0] == two[i][0] && s[1] == two[i][1]) { memcpy(k.op, two[i], 2); break; }
        if (!k.op[0]) k.op[0] = *s;
        s += strlen(k.op);
        push(ts, k);
    }
}

static int is_op(Toks *ts, const char *op)
{
    return ts->at < ts->n && ts->t[ts->at].kind == T_OP && !strcmp(ts->t[ts->at].op, op);
}

static long expr(Toks *ts, int prec);

static long unary(Toks *ts)
{
    if (ts->at >= ts->n) return 0;
    Tok *k = &ts->t[ts->at];
    if (k->kind == T_NUM) { ts->at++; return k->v; }
    ts->at++;
    switch (k->op[0]) {
    case '!': return !unary(ts);
    case '~': return ~unary(ts);
    case '-': return -unary(ts);
    case '+': return unary(ts);
    case '(': {
        long v = expr(ts, 0);
        if (is_op(ts, ")")) ts->at++;
        return v;
    }
    }
    return 0;
}

/* binary operators, loosest first */
static const char *LEVELS[][4] = {
    { "||" }, { "&&" }, { "|" }, { "^" }, { "&" }, { "==", "!=" },
    { "<", ">", "<=", ">=" }, { "<<", ">>" }, { "+", "-" }, { "*", "/", "%" },
};
#define NLEVELS (int)(sizeof LEVELS / sizeof *LEVELS)

static long apply(const char *op, long a, long b)
{
    if (!strcmp(op, "||")) return a || b;
    if (!strcmp(op, "&&")) return a && b;
    if (!strcmp(op, "|"))  return a | b;
    if (!strcmp(op, "^"))  return a ^ b;
    if (!strcmp(op, "&"))  return a & b;
    if (!strcmp(op, "==")) return a == b;
    if (!strcmp(op, "!=")) return a != b;
    if (!strcmp(op, "<"))  return a < b;
    if (!strcmp(op, ">"))  return a > b;
    if (!strcmp(op, "<=")) return a <= b;
    if (!strcmp(op, ">=")) return a >= b;
    if (!strcmp(op, "<<")) return a << b;
    if (!strcmp(op, ">>")) return a >> b;
    if (!strcmp(op, "+"))  return a + b;
    if (!strcmp(op, "-"))  return a - b;
    if (!strcmp(op, "*"))  return a * b;
    if (!strcmp(op, "/"))  return b ? a / b : 0;
    if (!strcmp(op, "%"))  return b ? a % b : 0;
    return 0;
}

static long expr(Toks *ts, int prec)
{
    if (prec == NLEVELS) return unary(ts);
    long v = expr(ts, prec + 1);
    for (;;) {
        const char *op = NULL;
        for (int i = 0; i < 4 && LEVELS[prec][i]; i++)
            if (is_op(ts, LEVELS[prec][i])) op = LEVELS[prec][i];
        if (!op) break;
        ts->at++;
        v = apply(op, v, expr(ts, prec + 1));
    }
    if (prec == 0 && is_op(ts, "?")) {
        ts->at++;
        long a = expr(ts, 0);
        if (is_op(ts, ":")) ts->at++;
        long b = expr(ts, 0);
        v = v ? a : b;
    }
    return v;
}

static long eval(Table *tab, const char *s)
{
    Toks ts = { 0 };
    lex(tab, s, &ts, 0);
    long v = expr(&ts, 0);
    free(ts.t);
    return v;
}

/* ---- lines ---- */

/* a directive's text with its comments taken out (they may run on) */
static char *strip_comments(const char *s, int *in_block)
{
    char *out = malloc(strlen(s) + 1), *o = out;
    while (*s) {
        if (*in_block) {
            if (s[0] == '*' && s[1] == '/') { *in_block = 0; s += 2; *o++ = ' '; }
            else s++;
        } else if (s[0] == '/' && s[1] == '*') { *in_block = 1; s += 2; }
        else if (s[0] == '/' && s[1] == '/') break;
        else *o++ = *s++;
    }
    *o = 0;
    return out;
}

/* whether the line ends inside a block comment, given how it began */
static int ends_in_comment(const char *s, int in_block)
{
    while (*s) {
        if (in_block) { if (s[0] == '*' && s[1] == '/') { in_block = 0; s += 2; continue; } }
        else if (s[0] == '/' && s[1] == '*') { in_block = 1; s += 2; continue; }
        else if (s[0] == '/' && s[1] == '/') return 0;
        s++;
    }
    return in_block;
}

int glsl_has_conditionals(const char *src)
{
    for (const char *p = src; (p = strchr(p, '#')) != NULL; p++) {
        const char *d = p + 1;
        while (*d == ' ' || *d == '\t') d++;
        if (!strncmp(d, "if", 2) || !strncmp(d, "elif", 4)) return 1;
    }
    return 0;
}

typedef struct { int parent, active, taken; } Cond;   /* an open #if */

char *glsl_resolve_conditionals(const char *src)
{
    static Table tab;                       /* one shader at a time, the game thread's */
    for (int i = 0; i < tab.n; i++) free(tab.m[i].value);
    tab.n = 0;

    size_t cap = strlen(src) + 1;
    char *out = malloc(cap), *o = out;
    Cond st[MAX_DEPTH];
    int depth = 0, active = 1, in_block = 0;

    const char *p = src;
    while (*p) {
        /* one logical line: continuations joined, newlines kept for the output */
        const char *start = p;
        size_t nl = 0;
        while (*p && !(*p == '\n' && (p == start || p[-1] != '\\'))) { if (*p == '\n') nl++; p++; }
        size_t len = (size_t)(p - start);
        int final_nl = *p == '\n';
        if (final_nl) { p++; nl++; }
        char *line = malloc(len + 1);
        size_t k = 0;
        for (size_t i = 0; i < len; i++) {
            if (start[i] == '\\' && i + 1 < len && start[i + 1] == '\n') { i++; continue; }
            if (start[i] != '\r') line[k++] = start[i];
        }
        line[k] = 0;

        const char *d = line;
        while (*d == ' ' || *d == '\t') d++;
        int directive = !in_block && *d == '#';
        int keep = active;                  /* the line goes out as it is */
        if (directive) {
            d++;
            while (*d == ' ' || *d == '\t') d++;
            const char *w = d;
            while (ident_char(*d)) d++;
            size_t wl = (size_t)(d - w);
            int ib = 0;
            char *rest = strip_comments(d, &ib);
            #define IS(x) (wl == strlen(x) && !memcmp(w, x, wl))
            if (IS("if") || IS("ifdef") || IS("ifndef")) {
                int v = 0;
                if (active) {
                    if (IS("if")) v = eval(&tab, rest) != 0;
                    else {
                        const char *n = rest;
                        while (isspace((unsigned char)*n)) n++;
                        const char *e = n;
                        while (ident_char(*e)) e++;
                        v = (find(&tab, n, (size_t)(e - n)) != NULL) == IS("ifdef");
                    }
                }
                if (depth < MAX_DEPTH) st[depth++] = (Cond){ active, active && v, active && v };
                active = active && v;
                keep = 0;
            } else if (IS("elif") || IS("else")) {
                if (depth > 0) {
                    Cond *s = &st[depth - 1];
                    int v = s->parent && !s->taken && (IS("else") || eval(&tab, rest) != 0);
                    s->active = v;
                    s->taken |= v;
                    active = v;
                }
                keep = 0;
            } else if (IS("endif")) {
                if (depth > 0) active = st[--depth].parent;
                keep = 0;
            } else if (active && (IS("define") || IS("undef"))) {
                const char *n = rest;
                while (isspace((unsigned char)*n)) n++;
                const char *e = n;
                while (ident_char(*e)) e++;
                if (IS("define")) define(&tab, n, (size_t)(e - n), e);
                else undef(&tab, n, (size_t)(e - n));
            }
            #undef IS
            free(rest);
        }
        in_block = ends_in_comment(line, in_block);

        /* out: the line (with its continuations re-joined) or nothing, and
         * as many newlines as it spanned */
        size_t need = (size_t)(o - out) + len + nl + 2;
        if (need > cap) { size_t at = (size_t)(o - out); cap = need * 2; out = realloc(out, cap); o = out + at; }
        if (keep) {
            for (size_t i = 0; i < len; i++) *o++ = start[i];
            if (final_nl) *o++ = '\n';
        } else {
            for (size_t i = 0; i < nl; i++) *o++ = '\n';
        }
        free(line);
    }
    *o = 0;
    return out;
}
