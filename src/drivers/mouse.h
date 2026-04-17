#ifndef MOUSE_H
#define MOUSE_H

#include "include/types.h"

/* PS/2 mouse driver. Standard 3-byte packet on IRQ12. Tracks an
 * accumulating screen-space cursor (clamped to a configurable
 * extent) plus a button bitmask (bit 0=L, 1=R, 2=M). Userspace
 * polls via SYS_MOUSE_POLL — the kernel never blocks on mouse
 * input.
 *
 * Coordinate convention: Y increases downward (matches VGA mode
 * 13h framebuffer layout), so the driver negates the raw PS/2
 * "up is positive" dy when integrating. */

void mouse_init(void);

/* Set the clamping extent for accumulated x/y. Call once after
 * entering a framebuffer mode so the cursor doesn't wander past
 * the visible area. Defaults to 320x200 on init. */
void mouse_set_extent(uint32_t width, uint32_t height);

void mouse_get_state(int32_t *x_out, int32_t *y_out, uint32_t *buttons_out);

#endif
