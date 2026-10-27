/* arbprog.h - assembly programs translated to GLSL (arbprog.c). */
#ifndef ARBPROG_H
#define ARBPROG_H

int  arb_active(void);
void arb_bind(GLenum target, GLuint id);
void arb_set_source(GLenum target, const char *src, int len);
const char *arb_get_source(GLenum target, GLuint id);
void arb_delete(GLenum target, GLuint id);
void arb_env(GLenum target, GLuint index, const float *v);
void arb_local(GLenum target, GLuint index, const float *v);
int  arb_enable(GLenum cap, int on);
int  arb_is_enabled(GLenum cap, int *out);
void arb_push_attrib(GLbitfield mask);
void arb_pop_attrib(void);
void arb_use_glsl(GLuint prog);
void arb_before_draw(void);

#endif
