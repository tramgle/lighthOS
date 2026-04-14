#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "include/types.h"

/* Special key codes. Values are non-ASCII (0x80+) so they don't collide
   with Ctrl-letter input — Ctrl-C (0x03) in particular is now usable
   as SIGINT without stepping on KEY_LEFT, which used to be 0x03. */
#define KEY_UP       0x81
#define KEY_DOWN     0x82
#define KEY_LEFT     0x83
#define KEY_RIGHT    0x84
#define KEY_HOME     0x85
#define KEY_END      0x86
#define KEY_CUP      0x91  /* Ctrl+Up    */
#define KEY_CDOWN    0x92  /* Ctrl+Down  */
#define KEY_CLEFT    0x93  /* Ctrl+Left  */
#define KEY_CRIGHT   0x94  /* Ctrl+Right */
#define KEY_DEL      0x95  /* Delete (forward) */

void keyboard_init(void);
char keyboard_getchar(void);
bool keyboard_has_key(void);

#endif
