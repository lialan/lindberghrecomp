/* solib.h - shared objects lifted ahead of time (tools/recomp/solib.py). */
#ifndef SOLIB_H
#define SOLIB_H
#include <stdint.h>

typedef struct {
    const char          *name;
    uint32_t             base, size;
    const unsigned char *image;          /* relocated, ready to copy */
    uint32_t             init;           /* DT_INIT, or 0 */
    uint32_t             init_array;     /* DT_INIT_ARRAY, or 0 */
    int                  init_count;
} SolibImage;

extern const SolibImage g_solib_images[];
extern const int        g_solib_count;

#endif
