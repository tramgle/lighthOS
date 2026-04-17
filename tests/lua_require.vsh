# Lua `require` is now registered (package + loadlib). Pure-Lua
# modules work (C modules still gated on LUA_USE_DLOPEN). Create a
# module on the ramfs at one of the default-path locations, require
# it, check its exported value.

# Drop a module in /scratch and point package.path at it. /lib/lua is
# already in the default path but may be pre-populated with multiboot
# modules; using /scratch keeps the test self-contained.
echo "return { ok = 42 }" > /scratch/modtest.lua

lua -e "package.path='/scratch/?.lua'; local m = require('modtest'); print(m.ok)" > /scratch/req
grep 42 /scratch/req | wc -l > /scratch/req_ok
assert lua.require.loaded 1 /scratch/req_ok

# Default package.path contains /lib/lua
lua -e "print(package.path)" > /scratch/ppath
grep "/lib/lua" /scratch/ppath | wc -l > /scratch/ppath_ok
assert lua.package.path 1 /scratch/ppath_ok

# C-module require: load /lib/luamod.so (built from user/libluamod).
# Its luaopen_luamod returns a table with answer() -> 42. This is the
# dlopen-via-ld.so + dlsym roundtrip that motivated making the Lua
# binary dynamic.
lua -e "local m = require('luamod'); print(m.answer())" > /scratch/creq
grep 42 /scratch/creq | wc -l > /scratch/creq_ok
assert lua.require.cmodule 1 /scratch/creq_ok
