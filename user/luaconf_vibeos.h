/* VibeOS overlay for Lua's luaconf.h. Gets pulled in via -include so
   it lands before the stock luaconf.h. Disables POSIX/Linux assumptions,
   picks C89-sized numbers so we avoid depending on runtime features our
   libc subset doesn't implement. */

#ifndef LUACONF_VIBEOS_H
#define LUACONF_VIBEOS_H

/* Force the C89 number path: long + double. Integer literals stay 32-bit
   (our long is int32), and double uses the x87 FPU which is still
   available under -mno-sse/-mno-mmx/-mno-sse2. */
#define LUA_USE_C89 1

/* No dynamic loader, no /etc/lua paths, no readline. */
#undef LUA_USE_POSIX
#undef LUA_USE_LINUX
#undef LUA_USE_READLINE
#undef LUA_USE_DLOPEN

/* luaconf.h includes <limits.h> and <stddef.h> unconditionally — both
   provided by user/libc/include. The rest of Lua includes standard C
   headers which we also provide. */

#endif
