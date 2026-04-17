#include "drivers/serial.h"
#include "drivers/keyboard.h"  /* KEY_UP, KEY_DOWN, etc. */
#include "kernel/isr.h"
#include "kernel/pic.h"
#include "kernel/process.h"    /* process_kill_foreground */
#include "include/io.h"

#define SERIAL_BUF_SIZE 256

static char serial_buffer[SERIAL_BUF_SIZE];
static volatile uint32_t serial_read_idx;
static volatile uint32_t serial_write_idx;
static uint32_t line_len;       /* chars on the current input line */
static int serial_raw_mode;     /* 1 = caller does its own echo + line editing */

void serial_set_raw(int enable) {
    serial_raw_mode = enable ? 1 : 0;
    /* Force line accounting back to zero on the transition. Any
       caller flipping modes mid-input probably doesn't want the
       kernel counter to leak across. */
    line_len = 0;
}

int serial_get_raw(void) { return serial_raw_mode; }

/* Terminal window size cache. Userspace probes via CSI-6n and calls
   serial_set_winsize to update. Defaults to the historical 24×80
   minimum — programs that read this before anyone has probed get a
   sane-but-conservative answer. */
static uint16_t win_rows = 24;
static uint16_t win_cols = 80;

void serial_get_winsize(uint16_t *rows, uint16_t *cols) {
    if (rows) *rows = win_rows;
    if (cols) *cols = win_cols;
}

void serial_set_winsize(uint16_t rows, uint16_t cols) {
    if (rows > 0) win_rows = rows;
    if (cols > 0) win_cols = cols;
}

static int serial_is_transmit_empty(void) {
    return inb(SERIAL_COM1 + 5) & 0x20;
}

static void serial_enqueue(char c) {
    uint32_t next = (serial_write_idx + 1) % SERIAL_BUF_SIZE;
    if (next != serial_read_idx) {
        serial_buffer[serial_write_idx] = c;
        serial_write_idx = next;
    }
}

/* CSI parser state machine.
   state == 0: normal
   state == 1: got ESC, awaiting '[' (or lone-ESC deliver)
   state == 2: inside a CSI — accumulating parameters and intermediate
               bytes until a final byte in 0x40..0x7E arrives. We only
               actually care about the second numeric parameter (the
               modifier: 5=Ctrl, 2=Shift, 3=Alt) and the final letter. */
static uint8_t esc_state;
static uint16_t csi_mod;    /* modifier (2nd CSI arg); 0 = none */
static uint8_t  csi_arg_idx;
static uint16_t csi_arg;    /* accumulator for current numeric arg */

static void csi_reset(void) {
    csi_mod = 0;
    csi_arg = 0;
    csi_arg_idx = 0;
}

static void csi_finish(char final) {
    /* For arrow/home/end keys, a CSI of the form ESC[1;5A means arg0=1,
       arg1=5 (modifier: Ctrl). Plain ESC[A has no args; arg1 defaults
       to 0. Modifier 5 = Ctrl; 2 = Shift; 3 = Alt; anything else treat
       as unmodified (good enough for a hobby OS). */
    /* Whatever arg we were accumulating when the final byte arrived
       is really arg[csi_arg_idx]; for the ESC[1;5A case that's arg[1]
       when we hit 'A'. Commit it. */
    if (csi_arg_idx == 1) csi_mod = csi_arg;
    int ctrl = (csi_mod == 5);

    switch (final) {
    case 'A': serial_enqueue(ctrl ? KEY_CUP    : KEY_UP);    return;
    case 'B': serial_enqueue(ctrl ? KEY_CDOWN  : KEY_DOWN);  return;
    case 'C': serial_enqueue(ctrl ? KEY_CRIGHT : KEY_RIGHT); return;
    case 'D': serial_enqueue(ctrl ? KEY_CLEFT  : KEY_LEFT);  return;
    case 'H': serial_enqueue(KEY_HOME); return;
    case 'F': serial_enqueue(KEY_END);  return;
    case '~':
        /* Tilde sequences: ESC[1~=Home, ESC[3~=Delete, ESC[4~=End,
           ESC[5~=PgUp, ESC[6~=PgDn. arg0 at csi_arg_idx==0 tells which. */
        {
            uint16_t code = (csi_arg_idx == 0) ? csi_arg : 0;
            switch (code) {
            case 1: serial_enqueue(KEY_HOME); return;
            case 3: serial_enqueue(KEY_DEL);  return;
            case 4: serial_enqueue(KEY_END);  return;
            /* 5/6 PgUp/PgDn: drop for now. */
            }
        }
        return;
    }
    /* Unknown CSI — silently drop. */
}

static registers_t *serial_callback(registers_t *regs) {
    while (inb(SERIAL_COM1 + 5) & 0x01) {  /* data ready */
        char c = inb(SERIAL_COM1);

        if (esc_state == 1) {
            if (c == '[') { esc_state = 2; csi_reset(); continue; }
            /* Lone ESC — deliver it; fall through to handle `c`. */
            serial_enqueue(0x1B);
            esc_state = 0;
        } else if (esc_state == 2) {
            if (c >= '0' && c <= '9') {
                csi_arg = csi_arg * 10 + (c - '0');
                continue;
            }
            if (c == ';') {
                /* Commit the arg we just finished parsing. arg[0] goes
                   away — we only care about arg[1] (modifier), which we
                   capture in csi_finish() when the final byte comes in. */
                csi_arg_idx++;
                csi_arg = 0;
                continue;
            }
            /* Final byte — commits the CSI. */
            if (c >= 0x40 && c <= 0x7E) {
                csi_finish(c);
                esc_state = 0;
                continue;
            }
            /* Ignore intermediate bytes; keep accumulating. */
            continue;
        }

        if (c == 0x1B) { esc_state = 1; continue; }

        /* Ctrl-C (0x03) — send SIGINT to the foreground group.
           Ctrl-Z (0x1A) — send SIGSTOP to the foreground group so
           the shell can reclaim the terminal and manage the stopped
           job. Raw bytes are swallowed either way. */
        if (c == 0x03) {
            process_kill_foreground();
            continue;
        }
        if (c == 0x1A) {
            process_stop_foreground();
            continue;
        }

        if (c == '\r') c = '\n';
        if (c == 0x7F) c = '\b';

        /* Raw mode: user space handles echo + line editing. Pass the
           byte through verbatim (including '\b'), no kernel mirror. */
        if (serial_raw_mode) {
            serial_enqueue(c);
            continue;
        }

        /* Cooked mode: line-discipline-lite. Track how many bytes are
           live on the current line so backspace can't chew into the
           prompt. line_len counts chars typed since the last newline;
           a backspace at the start is swallowed. Reset on newline. */
        if (c == '\b') {
            if (line_len == 0) continue;
            line_len--;
            serial_enqueue(c);
            serial_putchar('\b');
            serial_putchar(' ');
            serial_putchar('\b');
            continue;
        }

        serial_enqueue(c);

        if (c == '\n') {
            line_len = 0;
            serial_putchar('\r');
            serial_putchar('\n');
        } else if (c >= 0x20 && c < 0x7F) {
            line_len++;
            serial_putchar(c);
        }
    }
    return regs;
}

void serial_init(void) {
    serial_read_idx = 0;
    serial_write_idx = 0;

    /* Deliberately leave the FIFO disabled (16450 mode). Toggling FCR
       bit 0 (FIFO enable) on QEMU resets the receive holding register,
       which loses whichever byte the host chardev has already delivered
       — a visible bug as a dropped first character on piped stdin. */
    outb(SERIAL_COM1 + 1, 0x00);  /* Disable interrupts during setup */
    outb(SERIAL_COM1 + 3, 0x80);  /* Enable DLAB */
    outb(SERIAL_COM1 + 0, 0x01);  /* Divisor 1 = 115200 baud */
    outb(SERIAL_COM1 + 1, 0x00);  /* High byte of divisor */
    outb(SERIAL_COM1 + 3, 0x03);  /* 8 bits, no parity, 1 stop */
    outb(SERIAL_COM1 + 4, 0x0B);  /* DTR + RTS + OUT2 (UART IRQ to PIC) */
}

void serial_init_irq(void) {
    isr_register_handler(36, serial_callback);  /* IRQ4 = INT 36 */
    outb(SERIAL_COM1 + 1, 0x01);  /* Enable receive data available interrupt */
    pic_clear_mask(4);  /* unmask IRQ4 */
    /* Drain anything that arrived in the receive register before we got
       around to enabling the IRQ. Without this, the UART won't re-assert
       its IRQ line for data that was already pending. */
    serial_callback((registers_t *)0);
}

void serial_putchar(char c) {
    while (!serial_is_transmit_empty());
    outb(SERIAL_COM1, c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putchar('\r');
        serial_putchar(*s++);
    }
}

bool serial_has_data(void) {
    return serial_read_idx != serial_write_idx;
}

char serial_getchar(void) {
    /* Syscall entry clears IF (via IA32_FMASK, matching the INT-gate
       behavior of INT 0x80). Bare `hlt` with IF=0 would wait for an
       NMI, so the UART IRQ could never wake us. `sti; hlt` is the
       standard atomic re-enable-and-wait pair — IF flips on just
       before the halt so any pending IRQ fires immediately. */
    while (!serial_has_data()) {
        __asm__ volatile ("sti; hlt; cli");
    }
    char c = serial_buffer[serial_read_idx];
    serial_read_idx = (serial_read_idx + 1) % SERIAL_BUF_SIZE;
    return c;
}
