#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "include/types.h"

/* Special key codes (using values 0x01-0x06 which aren't normal ASCII input) */
#define KEY_UP    0x01
#define KEY_DOWN  0x02
#define KEY_LEFT  0x03
#define KEY_RIGHT 0x04

void keyboard_init(void);
char keyboard_getchar(void);
bool keyboard_has_key(void);

#endif
