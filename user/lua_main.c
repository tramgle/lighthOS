/* Scaled-down Lua standalone interpreter for LighthOS.
 * Supports:
 *   lua                   — interactive REPL
 *   lua <script.lua> [args...] — run a file (args exposed as `arg`)
 *   lua -                 — read script from stdin
 *   lua -e "stmt"         — execute a string
 *   lua -i                — force REPL (after -e or a script)
 *   lua -v                — print version + exit
 *   lua --                — end option processing
 * No signal handling, no readline, no -l library loading (needs
 * dlfcn + package — gated on those). */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static void l_message(const char *pname, const char *msg) {
    if (pname) fprintf(stderr, "%s: ", pname);
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
}

static int report(lua_State *L, int status) {
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        l_message("lua", msg ? msg : "(error object not a string)");
        lua_pop(L, 1);
    }
    return status;
}

static int msghandler(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (msg == NULL) {
        if (luaL_callmeta(L, 1, "__tostring") &&
            lua_type(L, -1) == LUA_TSTRING)
            return 1;
        msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    }
    luaL_traceback(L, L, msg, 1);
    return 1;
}

static int docall(lua_State *L, int narg, int nres) {
    int base = lua_gettop(L) - narg;
    lua_pushcfunction(L, msghandler);
    lua_insert(L, base);
    int status = lua_pcall(L, narg, nres, base);
    lua_remove(L, base);
    return status;
}

static int dochunk(lua_State *L, int status) {
    if (status == LUA_OK) status = docall(L, 0, 0);
    return report(L, status);
}

static int dofile_lua(lua_State *L, const char *name) {
    return dochunk(L, luaL_loadfile(L, name));
}

static int dostring(lua_State *L, const char *s, const char *name) {
    return dochunk(L, luaL_loadbuffer(L, s, strlen(s), name));
}

/* --- REPL --- */

static int incomplete(lua_State *L, int status) {
    if (status == LUA_ERRSYNTAX) {
        size_t lmsg;
        const char *msg = lua_tolstring(L, -1, &lmsg);
        if (lmsg >= 5 && strcmp(msg + lmsg - 5, "<eof>") == 0) {
            lua_pop(L, 1);
            return 1;
        }
    }
    return 0;
}

static int readline_repl(char *buf, int max, const char *prompt) {
    fputs(prompt, stdout);
    fflush(stdout);
    int pos = 0;
    while (pos < max - 1) {
        int c = fgetc(stdin);
        if (c == EOF) {
            if (pos == 0) return -1;  /* EOF at start of line */
            break;
        }
        if (c == '\n') break;
        /* The kernel serial driver is in cooked mode: it echoes every
           printable byte, handles backspace, and collapses \r→\n
           before delivering bytes here. So we keep our buffer in
           sync but emit nothing — echoing here would double-print. */
        if (c == '\b' || c == 0x7f) {
            if (pos > 0) pos--;
            continue;
        }
        if (c >= ' ' && c < 127) {
            buf[pos++] = (char)c;
        }
    }
    buf[pos] = '\0';
    return pos;
}

/* Return codes from loadline:
     -1 = EOF, -2 = blank line, otherwise LUA_OK (0) or LUA_ERR*. */
#define LOADLINE_EOF    (-1)
#define LOADLINE_BLANK  (-2)

static int loadline(lua_State *L) {
    char line[512];
    int len = readline_repl(line, sizeof(line), "> ");
    if (len < 0) return LOADLINE_EOF;
    if (len == 0) return LOADLINE_BLANK;

    /* Try "return <line>" first so expressions print their value. */
    char wrapped[540];
    snprintf(wrapped, sizeof(wrapped), "return %s;", line);
    int status = luaL_loadbuffer(L, wrapped, strlen(wrapped), "=stdin");
    if (status == LUA_OK) return LUA_OK;
    lua_pop(L, 1);

    /* Fall back to raw line, possibly across multiple lines. */
    char buf[4096];
    size_t total = 0;
    if ((size_t)len + 1 < sizeof(buf)) {
        memcpy(buf, line, len);
        total = len;
    } else {
        return LUA_ERRSYNTAX;
    }

    for (;;) {
        status = luaL_loadbuffer(L, buf, total, "=stdin");
        if (!incomplete(L, status)) break;
        lua_pop(L, 1);  /* remove error msg */
        int more = readline_repl(line, sizeof(line), ">> ");
        if (more < 0) break;
        if (total + 1 + more >= sizeof(buf)) { status = LUA_ERRSYNTAX; break; }
        buf[total++] = '\n';
        memcpy(buf + total, line, more);
        total += more;
    }
    return status;
}

static void print_results(lua_State *L) {
    int n = lua_gettop(L);
    if (n == 0) return;
    luaL_checkstack(L, LUA_MINSTACK, "too many results to print");
    lua_getglobal(L, "print");
    lua_insert(L, 1);
    if (lua_pcall(L, n, 0, 0) != LUA_OK) {
        l_message("lua", lua_pushfstring(L, "error calling 'print' (%s)", lua_tostring(L, -1)));
    }
}

static void repl(lua_State *L) {
    fputs("LighthOS Lua " LUA_VERSION_MAJOR "." LUA_VERSION_MINOR
          " — :q to quit\n", stdout);
    fflush(stdout);
    for (;;) {
        int status = loadline(L);
        if (status == LOADLINE_EOF) break;
        if (status == LOADLINE_BLANK) continue;
        if (status == LUA_OK) {
            status = docall(L, 0, LUA_MULTRET);
        }
        if (status == LUA_OK) {
            print_results(L);
        } else {
            report(L, status);
        }
        lua_settop(L, 0);
    }
    fputc('\n', stdout);
}

/* Expose command-line args as the `arg` table:
     arg[0]    = script name (or program name if no script)
     arg[1..n] = script args
     arg[-1..] = arguments before the script (lua itself + any -e/-v). */
static void push_arg_table(lua_State *L, int argc, char **argv,
                           int script_idx) {
    lua_createtable(L, argc, 0);
    int first = (script_idx > 0) ? script_idx : argc;
    for (int i = 0; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - first);
    }
    lua_setglobal(L, "arg");
}

static int dostdin(lua_State *L) {
    /* Slurp stdin into a buffer, then load as a chunk. Bounded at
       64 KiB — plenty for `echo 'print(1+1)' | lua -`. */
    static char buf[64 * 1024];
    size_t n = 0;
    int c;
    while (n < sizeof(buf) - 1 && (c = fgetc(stdin)) != EOF) {
        buf[n++] = (char)c;
    }
    buf[n] = 0;
    return dochunk(L, luaL_loadbuffer(L, buf, n, "=stdin"));
}

int main(int argc, char **argv) {
    lua_State *L = luaL_newstate();
    if (!L) {
        l_message("lua", "cannot create state: out of memory");
        return 1;
    }
    luaL_openlibs(L);

    /* First pass: locate the script index (or `-` for stdin) so we
       can build the `arg` table BEFORE any -e chunks run. Standard
       Lua: -e / -l chunks see `arg` already populated. */
    int script_idx = 0;
    int saw_e = 0, saw_v = 0, want_interactive = 0, from_stdin = 0;
    int args_end = argc;
    for (int j = 1; j < argc; j++) {
        const char *a = argv[j];
        if (strcmp(a, "--") == 0) { args_end = j; if (j + 1 < argc) script_idx = j + 1; break; }
        if (strcmp(a, "-") == 0)  { args_end = j; from_stdin = 1; break; }
        if (a[0] != '-')          { args_end = j; script_idx = j; break; }
        if (strcmp(a, "-e") == 0 && j + 1 < argc) { j++; }
    }

    push_arg_table(L, argc, argv,
                   script_idx ? script_idx : (from_stdin ? args_end : argc));

    /* Second pass: execute option chunks in order, then the script. */
    for (int j = 1; j < args_end; j++) {
        const char *a = argv[j];
        if (strcmp(a, "-e") == 0 && j + 1 < argc) {
            saw_e = 1;
            if (dostring(L, argv[++j], "=(command line)") != LUA_OK) {
                lua_close(L);
                return 1;
            }
        } else if (strcmp(a, "-i") == 0) {
            want_interactive = 1;
        } else if (strcmp(a, "-v") == 0) {
            saw_v = 1;
            fputs("LighthOS Lua " LUA_VERSION_MAJOR "." LUA_VERSION_MINOR
                  "." LUA_VERSION_RELEASE "\n", stdout);
            fflush(stdout);
        } else {
            l_message("lua", "unknown option (use -e, -i, -v, -, or --)");
            lua_close(L);
            return 1;
        }
    }

    int rc = 0;
    if (from_stdin) {
        if (dostdin(L) != LUA_OK) rc = 1;
    } else if (script_idx > 0) {
        if (dofile_lua(L, argv[script_idx]) != LUA_OK) rc = 1;
    } else if (!saw_e && !saw_v && !want_interactive) {
        /* Bare `lua` with no action: drop into the REPL. If any of
           -e / -v / -i was used, don't auto-enter — the user has to
           ask for the REPL explicitly with -i. */
        want_interactive = 1;
    }
    if (want_interactive) repl(L);

    lua_close(L);
    return rc;
}
