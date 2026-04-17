/* Scaled-down luaL_openlibs for LighthOS. Opens only the libraries we
   support: base, package-less, no debug/coroutine/os/utf8. */

#include "lprefix.h"
#include <stddef.h>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

static const luaL_Reg loadedlibs[] = {
    { LUA_GNAME,       luaopen_base     },
    { LUA_LOADLIBNAME, luaopen_package  },
    { LUA_TABLIBNAME,  luaopen_table    },
    { LUA_STRLIBNAME,  luaopen_string   },
    { LUA_MATHLIBNAME, luaopen_math     },
    { LUA_IOLIBNAME,   luaopen_io       },
    { LUA_OSLIBNAME,   luaopen_os       },
    { LUA_COLIBNAME,   luaopen_coroutine },
    { LUA_UTF8LIBNAME, luaopen_utf8     },
    { LUA_DBLIBNAME,   luaopen_debug    },
    { NULL, NULL }
};

void luaL_openlibs(lua_State *L) {
    for (const luaL_Reg *lib = loadedlibs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }
}
