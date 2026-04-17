/* Minimal Lua C module: exports luaopen_luamod.
 * Loaded by `require 'luamod'` once the Lua binary is dynamic and
 * LUA_USE_DLOPEN is on. Exports one function, `luamod.answer()`,
 * which returns 42. Proves the full loadlib → dlopen → dlsym chain
 * over LighthOS's ld.so + dlfcn shim. */

#include "lua.h"
#include "lauxlib.h"

static int luamod_answer(lua_State *L) {
    lua_pushinteger(L, 42);
    return 1;
}

static const luaL_Reg luamod_lib[] = {
    { "answer", luamod_answer },
    { NULL, NULL }
};

int luaopen_luamod(lua_State *L) {
    luaL_newlib(L, luamod_lib);
    return 1;
}
