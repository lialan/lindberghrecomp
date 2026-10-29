/* glsl_pp.h - a shader's #if blocks decided as desktop GLSL would (glsl_pp.c) */
#pragma once

/* whether the source has any #if or #elif */
int glsl_has_conditionals(const char *src);
/* the source with its conditionals evaluated: a new string, same line count */
char *glsl_resolve_conditionals(const char *src);
