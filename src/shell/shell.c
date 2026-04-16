#include "shell/shell.h"
#include "shell/commands.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "drivers/vga.h"
#include "drivers/serial.h"
#include "drivers/keyboard.h"
#include "drivers/console.h"
#include "fs/vfs.h"

#define LINE_MAX   256
#define ARGC_MAX   16
#define HIST_MAX   32
#define HIST_FILE  "/.history"

static char cwd[SHELL_CWD_MAX];

/* Command history */
static char history[HIST_MAX][LINE_MAX];
static int  hist_count;
static int  hist_pos;      /* current browse position while pressing UP/DOWN */

static char shell_getchar(void) {
    char c;
    while (console_read(&c, 1) < 1)
        ;
    return c;
}

static void hist_add(const char *line) {
    if (line[0] == '\0') return;
    /* Don't add duplicates of the most recent entry */
    if (hist_count > 0 && strcmp(history[hist_count - 1], line) == 0) return;

    if (hist_count < HIST_MAX) {
        strncpy(history[hist_count], line, LINE_MAX - 1);
        history[hist_count][LINE_MAX - 1] = '\0';
        hist_count++;
    } else {
        /* Shift everything up, drop oldest */
        for (int i = 0; i < HIST_MAX - 1; i++) {
            memcpy(history[i], history[i + 1], LINE_MAX);
        }
        strncpy(history[HIST_MAX - 1], line, LINE_MAX - 1);
        history[HIST_MAX - 1][LINE_MAX - 1] = '\0';
    }
}

static void hist_load(void) {
    struct vfs_stat st;
    if (vfs_stat(HIST_FILE, &st) != 0) return;
    if (st.size == 0) return;

    char buf[LINE_MAX];
    off_t offset = 0;
    int line_pos = 0;

    while (offset < st.size && hist_count < HIST_MAX) {
        char c;
        if (vfs_read(HIST_FILE, &c, 1, offset) != 1) break;
        offset++;
        if (c == '\n' || line_pos >= LINE_MAX - 1) {
            buf[line_pos] = '\0';
            if (line_pos > 0) hist_add(buf);
            line_pos = 0;
        } else {
            buf[line_pos++] = c;
        }
    }
    /* Last line if no trailing newline */
    if (line_pos > 0) {
        buf[line_pos] = '\0';
        hist_add(buf);
    }
    serial_printf("[shell] Loaded %d history entries\n", hist_count);
}

void shell_save_history(void) {
    /* Best-effort: writes to whatever fs owns '/'. If that's an
       ephemeral ramfs (ISO-only boot), the file just doesn't
       survive — harmless. */
    vfs_unlink(HIST_FILE);
    vfs_create(HIST_FILE, VFS_FILE);
    off_t offset = 0;
    for (int i = 0; i < hist_count; i++) {
        int len = strlen(history[i]);
        vfs_write(HIST_FILE, history[i], len, offset);
        offset += len;
        vfs_write(HIST_FILE, "\n", 1, offset);
        offset += 1;
    }
}

/* Clear the current line on both VGA and serial */
static void clear_line(const char *prompt, int prompt_len, int line_len) {
    /* Move cursor back to start of input */
    for (int i = 0; i < line_len; i++) {
        vga_backspace();
        serial_putchar('\b');
    }
    /* Overwrite with spaces */
    for (int i = 0; i < line_len; i++) {
        vga_putchar(' ');
        serial_putchar(' ');
    }
    /* Move back again */
    for (int i = 0; i < line_len; i++) {
        vga_backspace();
        serial_putchar('\b');
    }
    (void)prompt;
    (void)prompt_len;
}

static int shell_readline(char *buf, int max, const char *prompt) {
    int pos = 0;
    hist_pos = hist_count;  /* start past end of history */

    while (pos < max - 1) {
        char c = shell_getchar();

        if (c == '\n') {
            buf[pos] = '\0';
            kprintf("\n");
            return pos;
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
                vga_backspace();
                serial_putchar('\b');
                serial_putchar(' ');
                serial_putchar('\b');
            }
        } else if (c == KEY_UP) {
            if (hist_pos > 0) {
                clear_line(prompt, strlen(prompt), pos);
                hist_pos--;
                strncpy(buf, history[hist_pos], max - 1);
                pos = strlen(buf);
                /* Redraw */
                for (int i = 0; i < pos; i++) {
                    vga_putchar(buf[i]);
                    serial_putchar(buf[i]);
                }
            }
        } else if (c == KEY_DOWN) {
            clear_line(prompt, strlen(prompt), pos);
            if (hist_pos < hist_count - 1) {
                hist_pos++;
                strncpy(buf, history[hist_pos], max - 1);
                pos = strlen(buf);
            } else {
                hist_pos = hist_count;
                pos = 0;
                buf[0] = '\0';
            }
            for (int i = 0; i < pos; i++) {
                vga_putchar(buf[i]);
                serial_putchar(buf[i]);
            }
        } else if (c >= ' ' && c < 127) {
            buf[pos++] = c;
            kprintf("%c", c);
        }
    }
    buf[pos] = '\0';
    return pos;
}

static int shell_parse(char *line, char **argv, int max_argc) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_argc) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* Resolve a path relative to cwd. If path starts with /, it's absolute. */
void shell_resolve_path(const char *input, char *output, int out_max) {
    if (input[0] == '/') {
        strncpy(output, input, out_max - 1);
        output[out_max - 1] = '\0';
        return;
    }

    /* Relative: cwd + "/" + input */
    if (strcmp(cwd, "/") == 0) {
        output[0] = '/';
        strncpy(output + 1, input, out_max - 2);
        output[out_max - 1] = '\0';
    } else {
        strncpy(output, cwd, out_max - 1);
        int len = strlen(output);
        if (len < out_max - 2) {
            output[len] = '/';
            strncpy(output + len + 1, input, out_max - len - 2);
        }
        output[out_max - 1] = '\0';
    }
}

const char *shell_get_cwd(void) {
    return cwd;
}

void shell_set_cwd(const char *path) {
    strncpy(cwd, path, SHELL_CWD_MAX - 1);
    cwd[SHELL_CWD_MAX - 1] = '\0';
}

void shell_run(void) {
    char line[LINE_MAX];
    char *argv[ARGC_MAX];

    strcpy(cwd, "/");
    hist_count = 0;

    /* Load persistent history if /disk is available */
    hist_load();

    for (;;) {
        /* Print prompt with cwd */
        kprintf("vibeos:%s$ ", cwd);

        int len = shell_readline(line, LINE_MAX, "vibeos:$ ");
        if (len == 0) continue;

        hist_add(line);

        int argc = shell_parse(line, argv, ARGC_MAX);
        if (argc == 0) continue;

        /* Look up command */
        int cmd_count;
        const command_t *cmds = commands_get_table(&cmd_count);
        bool found = false;
        for (int i = 0; i < cmd_count; i++) {
            if (strcmp(argv[0], cmds[i].name) == 0) {
                cmds[i].handler(argc, argv);
                found = true;
                break;
            }
        }

        if (!found) {
            kprintf("%s: command not found\n", argv[0]);
        }
    }
}
