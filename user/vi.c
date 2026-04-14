#include "syscall.h"
#include "ulib.h"

#define SCREEN_ROWS 24   /* rows 0-23 for text */
#define SCREEN_COLS 80
#define STATUS_ROW  24   /* row 24 = status bar */
#define MAX_LINES   512
#define MAX_LINE_LEN 256

enum mode { MODE_NORMAL, MODE_INSERT, MODE_COMMAND, MODE_SEARCH };

/* Document state */
static char lines[MAX_LINES][MAX_LINE_LEN];
static int num_lines;

/* Editor state */
static int cx, cy;           /* cursor column, row in document */
static int scroll_offset;    /* first visible line */
static enum mode mode;
static char filename[256];
static int modified;

/* --- Undo stack ---
   Each entry snapshots the pre-edit state. `undo_push()` is called
   right before any modifying command; `u` pops and restores. No redo
   in v1 — it bloats the design without a clear use-case for our
   small files.
   Memory: UNDO_CAP * sizeof(buffer). Keep CAP small (3 deep) so
   the BSS stays manageable (~384 KB). */
#define UNDO_CAP 3
typedef struct {
    char lines[MAX_LINES][MAX_LINE_LEN];
    int  num_lines;
    int  cx, cy;
} undo_snap_t;
static undo_snap_t undo_stack[UNDO_CAP];
static int undo_depth;  /* 0..UNDO_CAP entries currently valid */

static void undo_push(void) {
    /* If we're full, drop the oldest by shifting. It's memcpy-heavy
       but only happens once the stack saturates; in practice the cost
       is invisible for typical single-file edits. */
    if (undo_depth == UNDO_CAP) {
        for (int i = 0; i < UNDO_CAP - 1; i++) {
            undo_stack[i] = undo_stack[i + 1];
        }
        undo_depth = UNDO_CAP - 1;
    }
    undo_snap_t *s = &undo_stack[undo_depth++];
    memcpy(s->lines, lines, sizeof lines);
    s->num_lines = num_lines;
    s->cx = cx;
    s->cy = cy;
}

static int undo_pop(void) {
    if (undo_depth == 0) return 0;
    undo_snap_t *s = &undo_stack[--undo_depth];
    memcpy(lines, s->lines, sizeof lines);
    num_lines = s->num_lines;
    cx = s->cx;
    cy = s->cy;
    return 1;
}

/* Command-line buffer (for : and / commands) */
static char cmdbuf[64];
static int cmdlen;

/* Search state. `/pattern<CR>` remembers the pattern; `n`/`N` repeat
   forward/backward using `last_pattern`. */
static char last_pattern[64];
static int  last_pattern_len;

/* Yank register (single unnamed). `reg_linewise` distinguishes yy / dd
   (entire-line copies, paste inserts whole new lines) from y$
   (char-range from cursor, paste splices into the current line). */
static char reg_buf[MAX_LINES][MAX_LINE_LEN];
static int  reg_nlines;    /* 0 = empty */
static int  reg_linewise;
static int  reg_trailing_len;   /* for charwise: chars in last stored line */

/* --- Terminal helpers --- */

static void term_clear(void) {
    puts("\033[2J\033[H");
}

static void term_move(int row, int col) {
    /* \033[row;colH (1-based) */
    char buf[16];
    int i = 0;
    buf[i++] = '\033'; buf[i++] = '[';
    /* Row digits */
    int r = row + 1;
    if (r >= 10) buf[i++] = '0' + r / 10;
    buf[i++] = '0' + r % 10;
    buf[i++] = ';';
    /* Col digits */
    int c = col + 1;
    if (c >= 10) buf[i++] = '0' + c / 10;
    buf[i++] = '0' + c % 10;
    buf[i++] = 'H';
    sys_write(1, buf, i);
}

static void term_clear_line(void) {
    puts("\033[K");
}

static char readkey(void) {
    char c;
    while (sys_read(0, &c, 1) < 1)
        sys_yield();
    return c;
}

/* --- Line operations --- */

static int linelen(int row) {
    return (int)strlen(lines[row]);
}

static void insert_line(int at) {
    if (num_lines >= MAX_LINES) return;
    for (int i = num_lines; i > at; i--) {
        memcpy(lines[i], lines[i - 1], MAX_LINE_LEN);
    }
    lines[at][0] = '\0';
    num_lines++;
}

static void delete_line(int at) {
    if (num_lines <= 1) {
        lines[0][0] = '\0';
        return;
    }
    for (int i = at; i < num_lines - 1; i++) {
        memcpy(lines[i], lines[i + 1], MAX_LINE_LEN);
    }
    num_lines--;
}

/* --- File I/O --- */

static void load_file(const char *path) {
    num_lines = 0;
    lines[0][0] = '\0';

    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        /* New file */
        num_lines = 1;
        return;
    }

    char buf[256];
    int32_t n;
    int col = 0;
    num_lines = 1;
    lines[0][0] = '\0';

    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                lines[num_lines - 1][col] = '\0';
                if (num_lines < MAX_LINES) {
                    num_lines++;
                    col = 0;
                    lines[num_lines - 1][0] = '\0';
                }
            } else {
                if (col < MAX_LINE_LEN - 1) {
                    lines[num_lines - 1][col++] = buf[i];
                    lines[num_lines - 1][col] = '\0';
                }
            }
        }
    }
    sys_close(fd);

    /* Remove trailing empty line if file ended with \n */
    if (num_lines > 1 && lines[num_lines - 1][0] == '\0') {
        num_lines--;
    }
    if (num_lines == 0) {
        num_lines = 1;
        lines[0][0] = '\0';
    }
}

static int save_file(const char *path) {
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;

    for (int i = 0; i < num_lines; i++) {
        int len = linelen(i);
        if (len > 0) sys_write(fd, lines[i], len);
        sys_write(fd, "\n", 1);
    }
    sys_close(fd);
    modified = 0;
    return 0;
}

/* --- Rendering --- */

static void render(void) {
    term_clear();

    for (int row = 0; row < SCREEN_ROWS; row++) {
        int doc_line = scroll_offset + row;
        term_move(row, 0);
        if (doc_line < num_lines) {
            int len = linelen(doc_line);
            if (len > SCREEN_COLS) len = SCREEN_COLS;
            if (len > 0) sys_write(1, lines[doc_line], len);
        } else {
            putchar('~');
        }
    }

    /* Status bar */
    term_move(STATUS_ROW, 0);
    const char *mode_str = "NORMAL";
    if (mode == MODE_INSERT) mode_str = "INSERT";
    else if (mode == MODE_COMMAND) mode_str = "COMMAND";

    printf("-- %s -- %s%s  L%d/%d C%d",
           mode_str,
           filename[0] ? filename : "[No Name]",
           modified ? " [+]" : "",
           cy + 1, num_lines, cx + 1);
    term_clear_line();

    /* Position cursor */
    int screen_row = cy - scroll_offset;
    term_move(screen_row, cx);
}

static void scroll_to_cursor(void) {
    if (cy < scroll_offset) scroll_offset = cy;
    if (cy >= scroll_offset + SCREEN_ROWS) scroll_offset = cy - SCREEN_ROWS + 1;
}

/* --- Search ---
   Naive substring match walking the document forward from (cy, cx+1)
   wrapping at EOF back to (0, 0). On match, move cursor there. `dir`
   is +1 for forward (used by / and n) or -1 for backward (N).
   Returns 1 if found. */
static int do_search(int dir) {
    if (last_pattern_len == 0) return 0;

    int start_row = cy;
    int start_col = cx + dir;   /* look past current position */

    int row = start_row;
    int col = start_col;

    for (int steps = 0; steps <= num_lines + 1; steps++) {
        /* Clamp col into the current row's range. */
        int ll = linelen(row);
        if (dir > 0) {
            if (col < 0) col = 0;
            if (col > ll) { row++; col = 0; }
            if (row >= num_lines) { row = 0; col = 0; }
        } else {
            if (col < 0) { row--; if (row < 0) row = num_lines - 1; col = linelen(row); }
        }

        /* Scan this row from col for the pattern. */
        ll = linelen(row);
        if (dir > 0) {
            for (int i = col; i + last_pattern_len <= ll; i++) {
                if (strncmp(&lines[row][i], last_pattern, last_pattern_len) == 0) {
                    cy = row; cx = i; return 1;
                }
            }
            /* Not found here; advance to next row. */
            row++; col = 0;
            if (row >= num_lines) { row = 0; }
        } else {
            for (int i = (col < ll - last_pattern_len ? col : ll - last_pattern_len); i >= 0; i--) {
                if (strncmp(&lines[row][i], last_pattern, last_pattern_len) == 0) {
                    cy = row; cx = i; return 1;
                }
            }
            row--; if (row < 0) row = num_lines - 1;
            col = linelen(row);
        }
        if (row == start_row && dir > 0 && col > start_col) break;
    }
    return 0;
}

/* --- Input handling --- */

static void handle_normal(char c) {
    switch (c) {
    case 'h': /* left */
        if (cx > 0) cx--;
        break;
    case 'l': /* right */
        if (cx < linelen(cy)) cx++;
        break;
    case 'k': /* up */
        if (cy > 0) { cy--; if (cx > linelen(cy)) cx = linelen(cy); }
        break;
    case 'j': /* down */
        if (cy < num_lines - 1) { cy++; if (cx > linelen(cy)) cx = linelen(cy); }
        break;
    case 'i': /* insert mode */
        undo_push();
        mode = MODE_INSERT;
        break;
    case 'a': /* append */
        undo_push();
        if (cx < linelen(cy)) cx++;
        mode = MODE_INSERT;
        break;
    case 'o': /* open line below */
        undo_push();
        insert_line(cy + 1);
        cy++;
        cx = 0;
        mode = MODE_INSERT;
        modified = 1;
        break;
    case 'O': /* open line above */
        undo_push();
        insert_line(cy);
        cx = 0;
        mode = MODE_INSERT;
        modified = 1;
        break;
    case 'x': { /* delete char under cursor */
        int len = linelen(cy);
        if (len > 0 && cx < len) {
            undo_push();
            for (int i = cx; i < len; i++) lines[cy][i] = lines[cy][i + 1];
            if (cx >= linelen(cy) && cx > 0) cx--;
            modified = 1;
        }
        break;
    }
    case 'd': { /* dd = delete (cut) line */
        char next = readkey();
        if (next == 'd') {
            undo_push();
            /* Yank into the register before deleting so `dd`+`p`
               acts as cut-and-paste. */
            memcpy(reg_buf[0], lines[cy], MAX_LINE_LEN);
            reg_nlines = 1;
            reg_linewise = 1;
            delete_line(cy);
            if (cy >= num_lines) cy = num_lines - 1;
            if (cy < 0) cy = 0;
            if (cx > linelen(cy)) cx = linelen(cy);
            modified = 1;
        }
        break;
    }
    case 'u': /* undo most recent change */
        if (undo_pop()) modified = 1;
        break;
    case 'y': { /* yy = yank line, y$ = yank to EOL */
        char next = readkey();
        if (next == 'y') {
            memcpy(reg_buf[0], lines[cy], MAX_LINE_LEN);
            reg_nlines = 1;
            reg_linewise = 1;
        } else if (next == '$') {
            int len = linelen(cy);
            int n = len - cx;
            if (n < 0) n = 0;
            for (int i = 0; i < n; i++) reg_buf[0][i] = lines[cy][cx + i];
            reg_buf[0][n] = '\0';
            reg_nlines = 1;
            reg_linewise = 0;
            reg_trailing_len = n;
        }
        break;
    }
    case 'p': { /* put after cursor */
        if (reg_nlines == 0) break;
        undo_push();
        if (reg_linewise) {
            for (int i = 0; i < reg_nlines; i++) {
                insert_line(cy + 1 + i);
                memcpy(lines[cy + 1 + i], reg_buf[i], MAX_LINE_LEN);
            }
            cy += 1;
            cx = 0;
        } else {
            /* Splice reg_buf[0] into current line after cx. */
            int len = linelen(cy);
            int rlen = reg_trailing_len;
            if (len + rlen < MAX_LINE_LEN - 1) {
                /* Shift tail right. */
                for (int i = len; i >= cx + 1; i--) lines[cy][i + rlen] = lines[cy][i];
                for (int i = 0; i < rlen; i++) lines[cy][cx + 1 + i] = reg_buf[0][i];
                lines[cy][len + rlen] = '\0';
                cx += rlen;
            }
        }
        modified = 1;
        break;
    }
    case 'P': { /* put before cursor */
        if (reg_nlines == 0) break;
        undo_push();
        if (reg_linewise) {
            for (int i = 0; i < reg_nlines; i++) {
                insert_line(cy + i);
                memcpy(lines[cy + i], reg_buf[i], MAX_LINE_LEN);
            }
            cx = 0;
        } else {
            int len = linelen(cy);
            int rlen = reg_trailing_len;
            if (len + rlen < MAX_LINE_LEN - 1) {
                for (int i = len; i >= cx; i--) lines[cy][i + rlen] = lines[cy][i];
                for (int i = 0; i < rlen; i++) lines[cy][cx + i] = reg_buf[0][i];
                lines[cy][len + rlen] = '\0';
            }
        }
        modified = 1;
        break;
    }
    case ':': /* command mode */
        mode = MODE_COMMAND;
        cmdlen = 0;
        cmdbuf[0] = '\0';
        break;
    case '/': /* search-pattern entry */
        mode = MODE_SEARCH;
        cmdlen = 0;
        cmdbuf[0] = '\0';
        break;
    case 'n': /* repeat last search forward */
        do_search(+1);
        break;
    case 'N': /* repeat last search backward */
        do_search(-1);
        break;
    case '0': /* beginning of line */
        cx = 0;
        break;
    case '$': /* end of line */
        cx = linelen(cy);
        if (cx > 0) cx--;
        break;
    case 'G': /* go to last line */
        cy = num_lines - 1;
        if (cx > linelen(cy)) cx = linelen(cy);
        break;
    /* Arrow keys from serial/keyboard driver: KEY_UP=0x81 etc. */
    case (char)0x81: if (cy > 0) { cy--; if (cx > linelen(cy)) cx = linelen(cy); } break;
    case (char)0x82: if (cy < num_lines - 1) { cy++; if (cx > linelen(cy)) cx = linelen(cy); } break;
    case (char)0x83: if (cx > 0) cx--; break;
    case (char)0x84: if (cx < linelen(cy)) cx++; break;
    }
}

static void handle_insert(char c) {
    if (c == 0x1B || c == 0x1b) {
        /* ESC -> back to normal */
        mode = MODE_NORMAL;
        if (cx > 0 && cx >= linelen(cy)) cx--;
        return;
    }
    if (c == '\b') {
        if (cx > 0) {
            int len = linelen(cy);
            for (int i = cx - 1; i < len; i++) lines[cy][i] = lines[cy][i + 1];
            cx--;
            modified = 1;
        } else if (cy > 0) {
            /* Join with previous line */
            int prev_len = linelen(cy - 1);
            strcat(lines[cy - 1], lines[cy]);
            delete_line(cy);
            cy--;
            cx = prev_len;
            modified = 1;
        }
        return;
    }
    if (c == '\n') {
        /* Split line at cursor */
        insert_line(cy + 1);
        strcpy(lines[cy + 1], lines[cy] + cx);
        lines[cy][cx] = '\0';
        cy++;
        cx = 0;
        modified = 1;
        return;
    }
    /* Arrow keys in insert mode */
    if (c == (char)0x81) { if (cy > 0) { cy--; if (cx > linelen(cy)) cx = linelen(cy); } return; }
    if (c == (char)0x82) { if (cy < num_lines - 1) { cy++; if (cx > linelen(cy)) cx = linelen(cy); } return; }
    if (c == (char)0x83) { if (cx > 0) cx--; return; }
    if (c == (char)0x84) { if (cx < linelen(cy)) cx++; return; }

    /* Insert printable character */
    if (c >= ' ' && c < 127) {
        int len = linelen(cy);
        if (len < MAX_LINE_LEN - 1) {
            for (int i = len + 1; i > cx; i--) lines[cy][i] = lines[cy][i - 1];
            lines[cy][cx] = c;
            cx++;
            modified = 1;
        }
    }
}

static int handle_command(void) {
    /* Returns 1 if editor should quit */
    if (strcmp(cmdbuf, "q") == 0) {
        if (modified) {
            /* Show warning on status bar */
            return 0;  /* refuse to quit */
        }
        return 1;
    }
    if (strcmp(cmdbuf, "q!") == 0) {
        return 1;
    }
    if (strcmp(cmdbuf, "w") == 0) {
        if (filename[0]) {
            save_file(filename);
        }
        return 0;
    }
    if (strcmp(cmdbuf, "wq") == 0) {
        if (filename[0]) {
            save_file(filename);
        }
        return 1;
    }
    /* :e <file> */
    if (strncmp(cmdbuf, "e ", 2) == 0) {
        strncpy(filename, cmdbuf + 2, sizeof(filename) - 1);
        load_file(filename);
        cx = cy = scroll_offset = 0;
        modified = 0;
        return 0;
    }
    /* :w <file> */
    if (strncmp(cmdbuf, "w ", 2) == 0) {
        strncpy(filename, cmdbuf + 2, sizeof(filename) - 1);
        save_file(filename);
        return 0;
    }
    return 0;
}

/* --- Main --- */

int main(int argc, char **argv) {
    filename[0] = '\0';
    num_lines = 1;
    lines[0][0] = '\0';
    cx = cy = scroll_offset = 0;
    mode = MODE_NORMAL;
    modified = 0;

    /* If a filename was given on the command line, load it. */
    if (argc > 1 && argv[1] && argv[1][0]) {
        int i = 0;
        while (argv[1][i] && i < (int)sizeof(filename) - 1) {
            filename[i] = argv[1][i];
            i++;
        }
        filename[i] = '\0';
        load_file(filename);
    }

    for (;;) {
        scroll_to_cursor();
        render();

        char c = readkey();

        if (mode == MODE_NORMAL) {
            handle_normal(c);
        } else if (mode == MODE_INSERT) {
            handle_insert(c);
        } else if (mode == MODE_COMMAND) {
            if (c == '\n') {
                if (handle_command()) break;
                mode = MODE_NORMAL;
            } else if (c == '\b') {
                if (cmdlen > 0) cmdlen--;
                cmdbuf[cmdlen] = '\0';
            } else if (c == 0x1B) {
                mode = MODE_NORMAL;
            } else if (c >= ' ' && c < 127 && cmdlen < 62) {
                cmdbuf[cmdlen++] = c;
                cmdbuf[cmdlen] = '\0';
            }

            /* Render command line on status row while in command mode */
            term_move(STATUS_ROW, 0);
            putchar(':');
            puts(cmdbuf);
            term_clear_line();
        } else if (mode == MODE_SEARCH) {
            if (c == '\n') {
                /* Commit pattern and jump to first match forward. */
                if (cmdlen > 0) {
                    int i = 0;
                    while (i < cmdlen && i < (int)sizeof(last_pattern) - 1) {
                        last_pattern[i] = cmdbuf[i]; i++;
                    }
                    last_pattern[i] = '\0';
                    last_pattern_len = i;
                }
                do_search(+1);
                mode = MODE_NORMAL;
            } else if (c == '\b') {
                if (cmdlen > 0) cmdlen--;
                cmdbuf[cmdlen] = '\0';
            } else if (c == 0x1B) {
                mode = MODE_NORMAL;
            } else if (c >= ' ' && c < 127 && cmdlen < 62) {
                cmdbuf[cmdlen++] = c;
                cmdbuf[cmdlen] = '\0';
            }

            term_move(STATUS_ROW, 0);
            putchar('/');
            puts(cmdbuf);
            term_clear_line();
        }
    }

    term_clear();
    return 0;
}
