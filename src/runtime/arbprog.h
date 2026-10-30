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
void arb_attrib_array(unsigned index, int on);   /* glEnable/DisableVertexAttribArray */
GLuint arb_bound(GLenum target);     /* the program bound to a target */
void arb_rect_enable_seen(void);   /* gl4es: the game set a rectangle enable */

unsigned arb_bound_vp(void);       /* the programs a draw now would use (0: none) */
unsigned arb_bound_fp(void);

int arb_env_projection(float P[16]);   /* the projection in program.env[0..3], for stereo.c */

#endif
