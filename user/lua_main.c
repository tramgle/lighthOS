/* Scaled-down Lua standalone interpreter for VibeOS.
 * Supports:
 *   lua                  — interactive REPL
 *   lua <script.lua>     — run a file
 *   lua -e "stmt"        — execute a string
 * No signal handling, no readline, no -l library loading. */

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
        if (c == '\b' || c == 0x7f) {
            if (pos > 0) { pos--; fputs("\b \b", stdout); fflush(stdout); }
            continue;
        }
        if (c >= ' ' && c < 127) {
            buf[pos++] = (char)c;
            fputc(c, stdout);
            fflush(stdout);
        }
    }
    fputc('\n', stdout);
    fflush(stdout);
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
    fputs("VibeOS Lua " LUA_VERSION_MAJOR "." LUA_VERSION_MINOR
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

int main(int argc, char **argv) {
    lua_State *L = luaL_newstate();
    if (!L) {
        l_message("lua", "cannot create state: out of memory");
        return 1;
    }
    luaL_openlibs(L);

    /* Process args. */
    int script_idx = 0;
    int saw_e = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-e") == 0 && i + 1 < argc) {
            saw_e = 1;
            if (dostring(L, argv[++i], "=(command line)") != LUA_OK) {
                lua_close(L);
                return 1;
            }
        } else if (a[0] == '-' && a[1] != '\0') {
            l_message("lua", "unknown option (only -e supported)");
            lua_close(L);
            return 1;
        } else {
            script_idx = i;
            break;
        }
    }

    int rc = 0;
    if (script_idx > 0) {
        if (dofile_lua(L, argv[script_idx]) != LUA_OK) rc = 1;
    } else if (!saw_e) {
        repl(L);
    }
    /* If -e ran without a script and without -i, exit normally — matches
       standard lua behavior, and avoids "accidentally dropping into the
       REPL after executing a one-liner". */

    lua_close(L);
    return rc;
}
