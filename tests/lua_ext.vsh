# Lua extensions added post-port: debug lib, arg global, -v / -i / -- flags.

# debug library registered
lua -e "print(type(debug.traceback))" > /scratch/lua_dbg1
assert lua.dbg.tb function /scratch/lua_dbg1

lua -e "print(debug.getinfo(1, 'S').what)" > /scratch/lua_dbg2
assert lua.dbg.info main /scratch/lua_dbg2

# arg global: with -e only, arg[0] is the program name and #arg == 0
lua -e "print(#arg)" > /scratch/lua_arg1
assert lua.arg.empty 0 /scratch/lua_arg1

# -v prints a version banner containing "Lua"
lua -v > /scratch/lua_v
grep Lua /scratch/lua_v | wc -l > /scratch/lua_v_count
assert lua.v.banner 1 /scratch/lua_v_count
