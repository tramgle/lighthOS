#include "drivers/keyboard.h"
#include "kernel/isr.h"
#include "kernel/pic.h"
#include "include/io.h"

#define KBD_DATA_PORT 0x60
#define KBD_BUF_SIZE  256

static char kbd_buffer[KBD_BUF_SIZE];
static volatile uint32_t kbd_read_idx;
static volatile uint32_t kbd_write_idx;

static bool shift_held;
static bool caps_on;
static bool extended;  /* next scancode is an 0xE0 extended key */

/* Scancode set 1 -> ASCII lookup (unshifted) */
static const char scancode_lower[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/',0,
    '*', 0, ' ', 0,
    0,0,0,0,0,0,0,0,0,0, /* F1-F10 */
    0, 0, /* Num/Scroll lock */
    0,0,0,'-',0,0,0,'+',0,0,0,0,0, /* keypad */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};

/* Scancode set 1 -> ASCII lookup (shifted) */
static const char scancode_upper[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?',0,
    '*', 0, ' ', 0,
    0,0,0,0,0,0,0,0,0,0,
    0, 0,
    0,0,0,'-',0,0,0,'+',0,0,0,0,0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};

static void kbd_enqueue(char c) {
    uint32_t next = (kbd_write_idx + 1) % KBD_BUF_SIZE;
    if (next != kbd_read_idx) {
        kbd_buffer[kbd_write_idx] = c;
        kbd_write_idx = next;
    }
}

static registers_t *keyboard_callback(registers_t *regs) {
    uint8_t scancode = inb(KBD_DATA_PORT);

    /* Extended scancode prefix */
    if (scancode == 0xE0) {
        extended = true;
        return regs;
    }

    if (extended) {
        extended = false;
        if (scancode & 0x80) return regs;
        switch (scancode) {
        case 0x48: kbd_enqueue(KEY_UP);    return regs;
        case 0x50: kbd_enqueue(KEY_DOWN);  return regs;
        case 0x4B: kbd_enqueue(KEY_LEFT);  return regs;
        case 0x4D: kbd_enqueue(KEY_RIGHT); return regs;
        }
        return regs;
    }

    /* Key release */
    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) {
            shift_held = false;
        }
        return regs;
    }

    /* Key press */
    if (scancode == 0x2A || scancode == 0x36) {
        shift_held = true;
        return regs;
    }
    if (scancode == 0x3A) {
        caps_on = !caps_on;
        return regs;
    }

    bool use_upper = shift_held;
    char c = scancode_lower[scancode];

    if (caps_on && c >= 'a' && c <= 'z') {
        use_upper = !use_upper;
    }

    if (use_upper) {
        c = scancode_upper[scancode];
    }

    if (c) {
        kbd_enqueue(c);
    }
    return regs;
}

void keyboard_init(void) {
    kbd_read_idx = 0;
    kbd_write_idx = 0;
    shift_held = false;
    caps_on = false;
    extended = false;

    isr_register_handler(33, keyboard_callback);
    pic_clear_mask(1);  /* unmask IRQ1 */
}

bool keyboard_has_key(void) {
    return kbd_read_idx != kbd_write_idx;
}

char keyboard_getchar(void) {
    while (!keyboard_has_key()) {
        __asm__ volatile ("hlt");
    }
    char c = kbd_buffer[kbd_read_idx];
    kbd_read_idx = (kbd_read_idx + 1) % KBD_BUF_SIZE;
    return c;
}
