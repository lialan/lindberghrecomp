#ifndef GLSL_COMPAT_H
#define GLSL_COMPAT_H
/* NVIDIA's legacy scalar/vector and half syntax -> portable GLSL.
 * The caller owns the returned string. No game-specific substitutions. */
char *glsl_compat(const char *source);
#endif
