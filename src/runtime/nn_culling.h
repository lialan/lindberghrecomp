#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* NN's side-plane builder temporarily sees shorter focal lengths; the actual
 * render projection is restored bit-for-bit before returning to the caller.
 * One title is loaded per process. 1 disables; env override wins. */
float nn_culling_default_widen(void);
int nn_culling_install(const char *label,uint32_t builder,uint32_t projection_x,uint32_t projection_y,float default_widen);
#ifdef __cplusplus
}
#endif
