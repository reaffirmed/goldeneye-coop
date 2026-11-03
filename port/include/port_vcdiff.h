#ifndef PORT_VCDIFF_H
#define PORT_VCDIFF_H
#include <stddef.h>
#include <stdint.h>

/* Apply a VCDIFF / xdelta3 patch (no secondary compression) to src.
 * 1 = ok: *out is malloc'd (free it), *outlen its size.
 * 0 = refused or damaged: err says why. port_vcdiff.c */
int portVcdiffApply(const uint8_t *src, size_t srclen, const uint8_t *patch, size_t patchlen,
                    uint8_t **out, size_t *outlen, char *err, size_t errn);

#endif
