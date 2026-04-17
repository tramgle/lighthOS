/* LighthOS overlay for Lua's luaconf.h. Gets pulled in via -include so
   it lands before the stock luaconf.h. Disables POSIX/Linux assumptions,
   picks C89-sized numbers so we avoid depending on runtime features our
   libc subset doesn't implement. */

#ifndef LUACONF_LIGHTHOS_H
#define LUACONF_LIGHTHOS_H

/* Force the C89 number path: long + double. Integer literals stay 32-bit
   (our long is int32), and double uses the x87 FPU which is still
   available under -mno-sse/-mno-mmx/-mno-sse2. */
#define LUA_USE_C89 1

/* No /etc/lua paths, no readline. */
#undef LUA_USE_POSIX
#undef LUA_USE_LINUX
#undef LUA_USE_READLINE

/* LUA_USE_DLOPEN on: loadlib.c picks up dlopen/dlsym/dlclose/dlerror
   through <dlfcn.h> (provided by user/libc/include/dlfcn.h, wired to
   ld-lighthos.so.1's ops table). Requires the Lua binary to be
   dynamically linked — see Makefile's $(BUILD_USER)/lua rule. */
#define LUA_USE_DLOPEN

/* require() search paths. Stock Lua defaults to /usr/local/share/...
   which doesn't exist on our FS. Search /lib/lua/ and the cwd. The
   trailing "./?/init.lua" lets `require 'foo'` find `./foo/init.lua`
   too, matching the standard lua convention. C-module loading
   remains disabled (LUA_USE_DLOPEN not defined). */
#define LUA_PATH_DEFAULT  "/lib/lua/?.lua;/lib/lua/?/init.lua;./?.lua;./?/init.lua"
#define LUA_CPATH_DEFAULT "/lib/lua/?.so;/lib/?.so;./?.so"

/* io.popen: libvibc provides popen()/pclose() over pipe+fork+execve
   (user/libc/stdio.c). Override Lua's POSIX-gated l_popen so we get
   working io.popen without also dragging in fseeko / flockfile /
   fflush(NULL). */
#include <stdio.h>
#define l_popen(L, c, m)   ((void)(L), popen((c), (m)))
#define l_pclose(L, file)  ((void)(L), pclose((file)))

/* luaconf.h includes <limits.h> and <stddef.h> unconditionally — both
   provided by user/libc/include. The rest of Lua includes standard C
   headers which we also provide. */

#endif
