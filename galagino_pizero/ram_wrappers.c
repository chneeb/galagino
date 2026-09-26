/*
 * ram_wrappers.c - RAM copies of library functions used by core 1's DVI path
 *
 * Core 1 must never fetch code from flash: while core 0 streams large data
 * through the 16 KB XIP cache (the menu logos), a flash fetch on core 1 can
 * stall long enough to miss a DVI line (shown red). memset, memcpy and the
 * SDK's interp_save/interp_restore live in flash but are called for every
 * line (margins, HDMI audio packets, pico_lib's TMDS encoder). The linker
 * redirects them here via --wrap (see CMakeLists.txt), as msxemulator does
 * for its queue functions.
 *
 * Built with -fno-tree-loop-distribute-patterns so GCC doesn't turn these
 * loops back into memset/memcpy calls.
 */

#include <stddef.h>
#include <stdint.h>

#include "pico.h"
#include "hardware/interp.h"

void *__not_in_flash_func(__wrap_memset)(void *dst, int c, size_t n) {
  uint8_t *d = dst;
  uint8_t v = (uint8_t)c;

  while(n && ((uintptr_t)d & 3)) { *d++ = v; n--; }

  uint32_t w = v * 0x01010101u;
  uint32_t *dw = (uint32_t *)d;
  while(n >= 16) { dw[0] = w; dw[1] = w; dw[2] = w; dw[3] = w; dw += 4; n -= 16; }
  while(n >= 4)  { *dw++ = w; n -= 4; }

  d = (uint8_t *)dw;
  while(n--) *d++ = v;
  return dst;
}

void *__not_in_flash_func(__wrap_memcpy)(void *dst, const void *src, size_t n) {
  uint8_t *d = dst;
  const uint8_t *s = src;

  if((((uintptr_t)d ^ (uintptr_t)s) & 3) == 0) {
    while(n && ((uintptr_t)d & 3)) { *d++ = *s++; n--; }
    uint32_t *dw = (uint32_t *)d;
    const uint32_t *sw = (const uint32_t *)s;
    while(n >= 16) { dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2]; dw[3] = sw[3]; dw += 4; sw += 4; n -= 16; }
    while(n >= 4)  { *dw++ = *sw++; n -= 4; }
    d = (uint8_t *)dw;
    s = (const uint8_t *)sw;
  }
  while(n--) *d++ = *s++;
  return dst;
}

// same as the SDK's hardware_interp versions, but in RAM
void __not_in_flash_func(__wrap_interp_save)(interp_hw_t *interp, interp_hw_save_t *saver) {
  saver->accum[0] = interp->accum[0];
  saver->accum[1] = interp->accum[1];
  saver->base[0] = interp->base[0];
  saver->base[1] = interp->base[1];
  saver->base[2] = interp->base[2];
  saver->ctrl[0] = interp->ctrl[0];
  saver->ctrl[1] = interp->ctrl[1];
}

void __not_in_flash_func(__wrap_interp_restore)(interp_hw_t *interp, interp_hw_save_t *saver) {
  interp->accum[0] = saver->accum[0];
  interp->accum[1] = saver->accum[1];
  interp->base[0] = saver->base[0];
  interp->base[1] = saver->base[1];
  interp->base[2] = saver->base[2];
  interp->ctrl[0] = saver->ctrl[0];
  interp->ctrl[1] = saver->ctrl[1];
}
