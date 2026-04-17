TARGET  = x86_64-elf
CC      = $(TARGET)-gcc
AS      = nasm
AR      = $(TARGET)-ar
LD      = $(TARGET)-gcc

DEBUG_KERNEL ?= 0
# x86_64 kernel flags:
#   -mcmodel=kernel  — kernel image lives in the negative high-half
#                      (0xFFFFFFFF80000000+); RIP-relative + sign-extended
#                      32-bit displacements reach everything.
#   -mno-red-zone    — kernel can't trust the 128-byte red zone; ISRs
#                      would clobber it.
#   -mno-sse*/-mmx   — no FPU/SIMD in kernel; save/restore not wired.
#   -fno-pic         — kernel is loaded at a fixed VA, not relocated.
CFLAGS  = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Isrc \
          -mcmodel=kernel -mno-red-zone -mno-sse -mno-mmx -mno-sse2 \
          -fno-pic -fno-pie -mgeneral-regs-only \
          -DDEBUG_KERNEL=$(DEBUG_KERNEL)
ASFLAGS = -f elf64
LDFLAGS = -T linker.ld -nostdlib -lgcc -ffreestanding -no-pie \
          -Wl,-z,max-page-size=0x1000 -mno-red-zone

C_SOURCES = $(shell find src -name '*.c')
# Exclude src/boot/disk/ — those are real-mode sources for the disk
# bootloader, assembled as flat binaries rather than linked into the
# kernel ELF.
S_SOURCES = $(shell find src -name '*.s' -not -path 'src/boot/disk/*')

C_OBJECTS = $(patsubst src/%.c, build/%.o, $(C_SOURCES))
S_OBJECTS = $(patsubst src/%.s, build/%.o, $(S_SOURCES))
OBJECTS   = $(C_OBJECTS) $(S_OBJECTS)

KERNEL_BIN = build/lighthos.bin
ISO        = build/lighthos.iso

# All build artifacts live under build/ so `.gitignore` can stay a
# single `build/` line and `make clean` is `rm -rf build`. Source
# files (user/*.c/h/s/ld, user/libc/**, user/ldso/**) stay in
# user/; nothing new is ever written there by the build.
BUILD_USER      = build/user
BUILD_USER_LIBC = build/user/libc
BUILD_USER_LDSO = build/user/ldso
DISK_IMG        = build/disk.img

# x86_64-ported user binaries. Grows each time a user program is
# brought back; replaces the legacy SIMPLE_USER set during the port.
# X64_USER_TEST  — test harness + in-system test binaries. Shipped on
#                  the test ISO only; kept out of the production
#                  ISO/bootdisk/disk image so those don't carry probe
#                  programs that a real user would never run.
# X64_USER_PROD  — everything a user would actually want at a prompt.
# X64_USER       — union; what X64_USER_TARGETS + per-binary linker
#                  rules iterate.
X64_USER_TEST   = runtests assert forktest fstest mmaptest envtest \
                  sigtest alarmtest \
                  test_pid test_fork test_fs test_stream test_pgroup test_xmm \
                  test_winsize
X64_USER_PROD   = hello shell bomb find rm stty \
                  echo cat wc head tail grep cp mv touch ls sleep mkdir \
                  mount umount chroot env \
                  strace \
                  hexdump pagemap regions \
                  flappy
X64_USER        = $(X64_USER_PROD) $(X64_USER_TEST)
# Binaries that need libvibc (stdio.h, printf, string.h) get their
# own linker rules below — not added to X64_USER so the generic
# pattern rule doesn't pick them up with the wrong linkage.
X64_USER_LIBC   = ps lsblk free install
# Simple single-source user targets built via the pattern rule below.
X64_USER_TARGETS = $(addprefix $(BUILD_USER)/,$(X64_USER))
X64_USER_PROD_TARGETS = $(addprefix $(BUILD_USER)/,$(X64_USER_PROD))
# User targets with their own build rules (libvibc/libulib linkage):
# lua + vi + the libc-linked tools below. Added to the ISO/disk
# staging list explicitly.
X64_USER_LIBC_TARGETS = $(addprefix $(BUILD_USER)/,$(X64_USER_LIBC))
X64_USER_EXTRA = $(BUILD_USER)/lua $(BUILD_USER)/vi $(X64_USER_LIBC_TARGETS)

X64_USER_CFLAGS = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -nostdlib \
                  -mno-red-zone -mno-sse -mno-mmx -mno-sse2 \
                  -mgeneral-regs-only -fno-stack-protector
# libvibc / lua need floats — they get SSE through X64_LIBC_CFLAGS
# below, which relies on CR4.OSFXSR being set in boot.s.
X64_LIBC_CFLAGS = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -nostdlib \
                  -mno-red-zone -fno-stack-protector \
                  -Iuser -Iuser/libc/include

$(BUILD_USER)/crt0.o: user/crt0.s
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

$(BUILD_USER)/%.o: user/%.c user/syscall_x64.h
	@mkdir -p $(dir $@)
	$(CC) $(X64_USER_CFLAGS) -c $< -o $@

# test_xmm uses movdqa inline asm on xmm0 so the compiler has to
# know SSE is allowed (operand constraint "xmm" and the clobber).
$(BUILD_USER)/test_xmm.o: user/test_xmm.c user/syscall_x64.h
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -c $< -o $@

# libulib.a: packaging of user/ulib.c. Compiled *with SSE* so
# libc clients that return doubles link cleanly against it.
$(BUILD_USER)/libulib/ulib.o: user/ulib.c user/ulib.h user/syscall_x64.h
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -c $< -o $@

build/sysroot/usr/lib/libulib.a: $(BUILD_USER)/libulib/ulib.o
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# libvibc.a: our subset libc. Compiled with SSE so strtod /
# snprintf's %f can return doubles via XMM0.
LIBVIBC_SRC = user/libc/mem.c user/libc/ctype.c user/libc/errno.c \
              user/libc/locale.c user/libc/string_extra.c \
              user/libc/strtod.c user/libc/snprintf.c user/libc/malloc.c \
              user/libc/stdio.c user/libc/misc.c user/libc/dlfcn.c
LIBVIBC_OBJS = $(patsubst user/libc/%.c,build/libvibc/%.o,$(LIBVIBC_SRC)) \
               build/libvibc/setjmp.o

build/libvibc/%.o: user/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -c $< -o $@

build/libvibc/setjmp.o: user/libc/setjmp.s
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

build/sysroot/usr/lib/libvibc.a: $(LIBVIBC_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# libtestdl.so.1 — a real ET_DYN shared library with two exported
# functions. Loaded by dlopentest via ld.so's dlopen iface.
build/sysroot/usr/lib/libtestdl.so.1: user/libtestdl/libtestdl.c
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -fPIC -c $< -o build/libvibc/libtestdl.o
	$(CC) -nostdlib -ffreestanding \
	      -Wl,-shared -Wl,-soname,libtestdl.so.1 \
	      -Wl,-z,max-page-size=0x1000 -Wl,--no-dynamic-linker \
	      -mno-red-zone -fno-stack-protector \
	      -o $@ build/libvibc/libtestdl.o -lgcc

# luamod.so — a Lua C module. Exports luaopen_luamod; its other
# symbol references (lua_pushinteger, luaL_newlib) are resolved at
# dlopen-time against the main /bin/lua binary, which is built with
# --export-dynamic so its Lua core symbols are visible.
build/sysroot/usr/lib/luamod.so: user/libluamod/libluamod.c
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -I$(LUA_SRC_DIR) -fPIC -fvisibility=default \
	      -c $< -o build/libvibc/libluamod.o
	$(CC) -nostdlib -ffreestanding \
	      -Wl,-shared -Wl,-soname,luamod.so \
	      -Wl,-z,max-page-size=0x1000 -Wl,--no-dynamic-linker \
	      -mno-red-zone -fno-stack-protector \
	      -o $@ build/libvibc/libluamod.o -lgcc

# libulib.so.1 — ET_DYN version of ulib. PIC build; exports the
# same API as libulib.a. Used by dynamic binaries via DT_NEEDED.
build/libulib/ulib.pic.o: user/ulib.c user/ulib.h user/syscall_x64.h
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -fPIC -fvisibility=default -c $< -o $@

build/sysroot/usr/lib/libulib.so.1: build/libulib/ulib.pic.o
	@mkdir -p $(dir $@)
	$(CC) -nostdlib -ffreestanding \
	      -Wl,-shared -Wl,-soname,libulib.so.1 \
	      -Wl,-z,max-page-size=0x1000 -Wl,--no-dynamic-linker \
	      -mno-red-zone -fno-stack-protector \
	      -o $@ $^ -lgcc
	@ln -sf libulib.so.1 build/sysroot/usr/lib/libulib.so

# libvibc.so.1 — PIC build of libvibc, depends on libulib.so.1.
LIBVIBC_PIC_OBJS = $(patsubst user/libc/%.c,build/libvibc/%.pic.o,$(LIBVIBC_SRC)) \
                   build/libvibc/setjmp.o

build/libvibc/%.pic.o: user/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -fPIC -fvisibility=default -c $< -o $@

build/sysroot/usr/lib/libvibc.so.1: $(LIBVIBC_PIC_OBJS) \
                                    build/sysroot/usr/lib/libulib.so.1
	@mkdir -p $(dir $@)
	$(CC) -nostdlib -ffreestanding \
	      -Wl,-shared -Wl,-soname,libvibc.so.1 \
	      -Wl,-z,max-page-size=0x1000 -Wl,--no-dynamic-linker \
	      -mno-red-zone -fno-stack-protector \
	      -o $@ $(LIBVIBC_PIC_OBJS) \
	      -Lbuild/sysroot/usr/lib -lulib -lgcc
	@ln -sf libvibc.so.1 build/sysroot/usr/lib/libvibc.so

# ld-lighthos.so.1 — the user-space runtime linker itself.
# Built as a static ET_EXEC at 0x40000000; links against libulib.a
# so it has its own syscall wrappers + strcmp/puts etc.
build/ldso/crt0_ldso.o: user/ldso/crt0_ldso.s
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

build/ldso/ld_main.o: user/ldso/ld_main.c user/ulib.h user/syscall_x64.h
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -c $< -o $@

build/sysroot/usr/lib/ld-lighthos.so.1: build/ldso/crt0_ldso.o \
                                        build/ldso/ld_main.o \
                                        user/ldso/ldso.ld \
                                        build/sysroot/usr/lib/libulib.a
	@mkdir -p $(dir $@)
	$(CC) -T user/ldso/ldso.ld -nostdlib -ffreestanding -static \
	      -Wl,--no-dynamic-linker -Wl,--build-id=none \
	      -mno-red-zone -fno-stack-protector \
	      -o $@ build/ldso/crt0_ldso.o build/ldso/ld_main.o \
	      -Wl,-Bstatic -Lbuild/sysroot/usr/lib -l:libulib.a -lgcc

# Lua: pull vendored sources from third_party/lua; compile + static
# link against libvibc + libulib. linit_lighthos + lua_main are the
# custom entry points.
LUA_SRC_DIR = third_party/lua/src
LUA_CORE = lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject \
           lopcodes lparser lstate lstring ltable ltm lundump lvm lzio
LUA_LIB  = lauxlib lbaselib lmathlib lstrlib ltablib liolib loslib lcorolib lutf8lib ldblib loadlib
LUA_OBJS = $(addprefix build/lua/, $(addsuffix .o, $(LUA_CORE) $(LUA_LIB)))

LUA_CFLAGS = $(X64_LIBC_CFLAGS) -I$(LUA_SRC_DIR) \
             -include user/luaconf_lighthos.h \
             -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
             -Wno-implicit-fallthrough -Wno-parentheses -Wno-empty-body \
             -Wno-maybe-uninitialized -Wno-unused-but-set-variable

build/lua/%.o: $(LUA_SRC_DIR)/%.c user/luaconf_lighthos.h
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

build/lua/lua_main.o: user/lua_main.c user/luaconf_lighthos.h
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

build/lua/linit_lighthos.o: user/linit_lighthos.c user/luaconf_lighthos.h
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

$(BUILD_USER)/lua: $(BUILD_USER)/crt0.o $(LUA_OBJS) \
                   build/lua/linit_lighthos.o build/lua/lua_main.o \
                   build/sysroot/usr/lib/libvibc.so.1 \
                   build/sysroot/usr/lib/libulib.so.1
	@mkdir -p $(dir $@)
	$(CC) -T user/user.ld -nostdlib -ffreestanding \
	      -Wl,--dynamic-linker=/lib/ld-lighthos.so.1 \
	      -Wl,--no-as-needed \
	      -Wl,--export-dynamic \
	      -Wl,-rpath-link,build/sysroot/usr/lib \
	      -mno-red-zone -fno-stack-protector \
	      -o $@ $(BUILD_USER)/crt0.o $(LUA_OBJS) \
	      build/lua/linit_lighthos.o build/lua/lua_main.o \
	      -Lbuild/sysroot/usr/lib -lvibc -lulib -lgcc

# Targets with their own recipes below (shell) get filtered out.
$(filter-out $(BUILD_USER)/shell,$(X64_USER_TARGETS)): $(BUILD_USER)/%: $(BUILD_USER)/crt0.o $(BUILD_USER)/%.o
	@mkdir -p $(dir $@)
	x86_64-elf-ld -T user/user.ld -nostdlib -o $@ $^

# vi needs stdio/string — compile with X64_LIBC_CFLAGS (which lets
# it see libvibc's <stdio.h>/<string.h>) and link libvibc.a +
# libulib.a statically, the same pattern lua uses.
build/user/vi.o: user/vi.c
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -Iuser/libc/include -c $< -o $@

$(BUILD_USER)/vi: $(BUILD_USER)/crt0.o $(BUILD_USER)/vi.o \
                  build/sysroot/usr/lib/libvibc.a \
                  build/sysroot/usr/lib/libulib.a
	@mkdir -p $(dir $@)
	x86_64-elf-ld -T user/user.ld -nostdlib -static \
	    -o $@ $(BUILD_USER)/crt0.o $(BUILD_USER)/vi.o \
	    build/sysroot/usr/lib/libvibc.a \
	    build/sysroot/usr/lib/libulib.a \
	    build/sysroot/usr/lib/libvibc.a

# Shell pulls in libulib.a for getenv + environ maintenance so the
# interactive $VAR expansion works.
$(BUILD_USER)/shell: $(BUILD_USER)/crt0.o $(BUILD_USER)/shell.o \
                     build/sysroot/usr/lib/libulib.a
	@mkdir -p $(dir $@)
	x86_64-elf-ld -T user/user.ld -nostdlib -static \
	    -o $@ $(BUILD_USER)/crt0.o $(BUILD_USER)/shell.o \
	    build/sysroot/usr/lib/libulib.a

# Per-binary rules for anything that #include's <stdio.h>/<string.h>
# from libvibc. Same linkage pattern as vi.
$(BUILD_USER)/ps.o $(BUILD_USER)/lsblk.o $(BUILD_USER)/free.o \
$(BUILD_USER)/install.o: $(BUILD_USER)/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -Iuser/libc/include -c $< -o $@

$(X64_USER_LIBC_TARGETS): $(BUILD_USER)/%: $(BUILD_USER)/crt0.o $(BUILD_USER)/%.o \
                          build/sysroot/usr/lib/libvibc.a \
                          build/sysroot/usr/lib/libulib.a
	@mkdir -p $(dir $@)
	x86_64-elf-ld -T user/user.ld -nostdlib -static \
	    -o $@ $(BUILD_USER)/crt0.o $(BUILD_USER)/$*.o \
	    build/sysroot/usr/lib/libvibc.a \
	    build/sysroot/usr/lib/libulib.a \
	    build/sysroot/usr/lib/libvibc.a

# Dynamic user binaries: PT_INTERP=/lib/ld-lighthos.so.1 + DT_NEEDED
# so the runtime linker resolves symbols at load time.
$(BUILD_USER)/dynhello: $(BUILD_USER)/crt0.o user/dynhello.c \
                       build/sysroot/usr/lib/libulib.so.1
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -c user/dynhello.c -o $(BUILD_USER)/dynhello.dyn.o
	$(CC) -T user/user.ld -nostdlib -ffreestanding \
	      -Wl,--dynamic-linker=/lib/ld-lighthos.so.1 \
	      -Wl,--no-as-needed \
	      -Wl,-rpath-link,build/sysroot/usr/lib \
	      -mno-red-zone -fno-stack-protector \
	      -o $@ $(BUILD_USER)/crt0.o $(BUILD_USER)/dynhello.dyn.o \
	      -Lbuild/sysroot/usr/lib -lulib -lgcc

$(BUILD_USER)/dyn_echo: $(BUILD_USER)/crt0.o user/dyn_echo.c \
                       build/sysroot/usr/lib/libvibc.so.1 \
                       build/sysroot/usr/lib/libulib.so.1
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -c user/dyn_echo.c -o $(BUILD_USER)/dyn_echo.dyn.o
	$(CC) -T user/user.ld -nostdlib -ffreestanding \
	      -Wl,--dynamic-linker=/lib/ld-lighthos.so.1 \
	      -Wl,--no-as-needed \
	      -Wl,-rpath-link,build/sysroot/usr/lib \
	      -mno-red-zone -fno-stack-protector \
	      -o $@ $(BUILD_USER)/crt0.o $(BUILD_USER)/dyn_echo.dyn.o \
	      -Lbuild/sysroot/usr/lib -lvibc -lulib -lgcc

$(BUILD_USER)/dlopentest: $(BUILD_USER)/crt0.o user/dlopentest.c \
                         build/sysroot/usr/lib/libulib.so.1
	@mkdir -p $(dir $@)
	$(CC) $(X64_LIBC_CFLAGS) -c user/dlopentest.c -o $(BUILD_USER)/dlopentest.dyn.o
	$(CC) -T user/user.ld -nostdlib -ffreestanding \
	      -Wl,--dynamic-linker=/lib/ld-lighthos.so.1 \
	      -Wl,--no-as-needed \
	      -Wl,-rpath-link,build/sysroot/usr/lib \
	      -mno-red-zone -fno-stack-protector \
	      -o $@ $(BUILD_USER)/crt0.o $(BUILD_USER)/dlopentest.dyn.o \
	      -Lbuild/sysroot/usr/lib -lulib -lgcc

.PHONY: x64-userland
x64-userland: $(X64_USER_TARGETS) $(X64_USER_LIBC_TARGETS) \
              $(BUILD_USER)/lua $(BUILD_USER)/vi \
              $(BUILD_USER)/dynhello $(BUILD_USER)/dyn_echo $(BUILD_USER)/dlopentest

.PHONY: all clean iso run run-disk run-vga run-vga-iso run-vga-both debug docker-build docker-run test docker-test iso-ready user-programs fix-perms docker-lua-compile bootdisk run-bootdisk docker-bootdisk docker-disk test-iso docker-test-iso test-disk docker-test-disk run-installed

all: $(ISO)

$(KERNEL_BIN): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/%.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(ISO): $(KERNEL_BIN) grub.cfg x64-userland \
        build/sysroot/usr/lib/libulib.so.1 \
        build/sysroot/usr/lib/libvibc.so.1 \
        build/sysroot/usr/lib/libtestdl.so.1 \
        build/sysroot/usr/lib/ld-lighthos.so.1
	@# Wipe any binaries that lingered from an earlier build — otherwise
	@# a file removed from X64_USER_PROD still rides along inside the ISO.
	@rm -rf build/iso/boot
	@mkdir -p build/iso/boot/grub build/iso/boot/lib
	cp $(KERNEL_BIN) build/iso/boot/lighthos.bin
	cp grub.cfg build/iso/boot/grub/grub.cfg
	@# Stage each ported user binary into the ISO so grub.cfg's
	@# `module /boot/<name> /bin/<name>` lines can drop them into
	@# ramfs at the declared paths. Production-only — test harness
	@# (runtests, assert, test_*, *test) stays on the test ISO.
	@for f in $(X64_USER_PROD_TARGETS) $(X64_USER_EXTRA) $(BUILD_USER)/dynhello \
	          $(BUILD_USER)/dyn_echo; do \
	    name=$$(basename $$f); \
	    if [ -x $$f ]; then cp $$f build/iso/boot/$$name; fi; \
	done
	cp build/sysroot/usr/lib/libulib.so.1     build/iso/boot/lib/libulib.so.1
	cp build/sysroot/usr/lib/libvibc.so.1     build/iso/boot/lib/libvibc.so.1
	cp build/sysroot/usr/lib/libtestdl.so.1   build/iso/boot/lib/libtestdl.so.1
	cp build/sysroot/usr/lib/ld-lighthos.so.1 build/iso/boot/lib/ld-lighthos.so.1
	grub-mkrescue -o $@ build/iso 2>/dev/null

# ---- Disk image with a single FAT32 partition at LBA 2048 ---------
# Layout: LBA 0=MBR, 1..62=stage2, 63..2047=kernel ELF, 2048+=FAT32.
# The partition is formatted + populated entirely from the host via
# mkfs.fat + mcopy — no QEMU round-trip needed. `make disk.img` is
# the one-stop entry point; `make patch-disk` is the legacy QEMU-based
# flow (still works for debugging but not needed for normal use).

DISK_SIZE_MB    = 64
DISK_FAT_OFFSET = 2048

$(DISK_IMG): x64-userland \
             build/sysroot/usr/lib/libulib.so.1 \
             build/sysroot/usr/lib/libvibc.so.1 \
             build/sysroot/usr/lib/libtestdl.so.1 \
             build/sysroot/usr/lib/ld-lighthos.so.1
	@mkdir -p $(dir $@)
	@echo "Creating $(DISK_SIZE_MB) MB disk with FAT32 at LBA $(DISK_FAT_OFFSET)"
	dd if=/dev/zero of=$@ bs=1M count=$(DISK_SIZE_MB) 2>/dev/null
	mkfs.fat -F 32 -n LIGHTHOS --offset=$(DISK_FAT_OFFSET) $@ >/dev/null 2>&1
	mmd -i $@@@$$(($(DISK_FAT_OFFSET)*512)) ::BIN 2>/dev/null || true
	mmd -i $@@@$$(($(DISK_FAT_OFFSET)*512)) ::LIB 2>/dev/null || true
	@for f in $(X64_USER_PROD_TARGETS) $(X64_USER_EXTRA) $(BUILD_USER)/dynhello \
	          $(BUILD_USER)/dyn_echo; do \
	    name=$$(basename $$f); \
	    mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o $$f ::BIN/$$name; \
	done
	@# Copy init: shell takes over as init on the bootdisk path, matching
	@# the grub-ISO layout. /BIN/init is how process.c's autorun fallback
	@# finds it; /BIN/shell is there so runtime lookups still resolve.
	mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o $(BUILD_USER)/shell ::BIN/init
	mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o build/sysroot/usr/lib/ld-lighthos.so.1 ::LIB/ld-lighthos.so.1
	mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o build/sysroot/usr/lib/libulib.so.1     ::LIB/libulib.so.1
	mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o build/sysroot/usr/lib/libvibc.so.1     ::LIB/libvibc.so.1
	mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o build/sysroot/usr/lib/libtestdl.so.1   ::LIB/libtestdl.so.1
	@echo "$@ ready ($(DISK_SIZE_MB) MB, FAT32)"

# Back-compat alias so `make disk.img` and `make docker-disk` still work.
disk.img: $(DISK_IMG)

docker-disk:
	$(DOCKER_RUN) make $(DISK_IMG)

# run targets check that ISO exists; build it via Docker if missing.
# -nographic routes all I/O (serial + monitor) to stdio. Ctrl-A X to quit.
run: iso-ready
	qemu-system-x86_64 -cdrom $(ISO) -nographic -m 128M

run-disk: iso-ready $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide \
		-nographic -m 128M

# run-vga boots the self-contained bootdisk with a VGA window as the
# primary display. PS/2 keyboard input comes in via FD_CONSOLE's
# keyboard branch; text goes out through console.c's ANSI decoder to
# vga_putchar. Serial is captured to build/serial.log for post-boot
# inspection (kernel log + boot output). The kernel winsize cache
# stays at the 24x80 default since no VT terminal is attached to
# answer CSI-6n — vi + other programs that honor it lay out in 24
# text rows + 1 status row = 25 = VGA text-mode geometry. Match.
run-vga:
	@if [ ! -f build/bootdisk.img ]; then \
	    echo "build/bootdisk.img missing — building a fresh one"; \
	    $(MAKE) docker-bootdisk; \
	else \
	    echo "using existing build/bootdisk.img (rm it to force a rebuild)"; \
	fi
	qemu-system-x86_64 -drive file=build/bootdisk.img,format=raw,if=ide \
		-m 128M -serial file:build/serial.log

# run-vga-iso: same idea but boots the ISO (not the self-hosting
# disk image). Useful when the bootdisk flow itself is what you're
# debugging and you want a known-good media to compare against.
run-vga-iso: iso-ready
	qemu-system-x86_64 -cdrom $(ISO) -m 128M -serial file:build/serial.log

# run-vga-both: VGA window AND an interactive serial on stdio.
# Anything CSI-6n-capable answering on stdio will populate the kernel
# winsize cache to the host terminal's real size — which then
# drives vi's layout even though the VGA display is still 80x25.
# Don't use this if you want the VGA editor to look right; use
# plain `run-vga` instead. Good for dev loops where you want to
# type at the serial side and watch output on VGA.
run-vga-both:
	@if [ ! -f build/bootdisk.img ]; then \
	    echo "build/bootdisk.img missing — building a fresh one"; \
	    $(MAKE) docker-bootdisk; \
	else \
	    echo "using existing build/bootdisk.img (rm it to force a rebuild)"; \
	fi
	qemu-system-x86_64 -drive file=build/bootdisk.img,format=raw,if=ide \
		-m 128M -serial stdio

debug: iso-ready
	qemu-system-x86_64 -cdrom $(ISO) -nographic -m 128M \
		-d int,cpu_reset -no-reboot -no-shutdown

# run-gdb uses QEMU's own -s -S gdbstub. Attach from another shell:
#   x86_64-elf-gdb build/lighthos.bin -ex 'target remote :1234'
# The kernel gdbstub.c was i386-only and got dropped in the port;
# QEMU's built-in stub is more than adequate for debugging.
run-gdb: iso-ready $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide \
		-m 128M -nographic -s -S -no-reboot

iso-ready:
	@if [ ! -f $(ISO) ]; then \
		echo "ISO missing, running docker-build..."; \
		$(MAKE) docker-build; \
	fi

# --- Legacy i386 userland (libc_test, lua, dynhello, shell, vi, ...)
# removed during the x86_64 port. The x64-userland target above builds
# the ported subset (currently hello, forktest, fstest). Full user
# rebuild returns when user/ulib.c, user/libc/*, and user/ldso/* are
# ported. Until then, `make all` goes no further than the kernel.

clean:
	rm -rf build
	@# Legacy cleanup for repos mid-migration from the old
	@# "build artifacts live next to sources" layout. Safe to remove
	@# once every developer has switched.
	@rm -f user/*.o user/libc/*.o user/ldso/*.o disk.img
	@for b in $(SIMPLE_USER) libc_test lua dynhello dyn_echo; do \
		rm -f user/$$b; \
	done

test: clean
	$(MAKE) CFLAGS="$(CFLAGS) -DRUN_TESTS" $(ISO)
	timeout 10 qemu-system-x86_64 -cdrom $(ISO) -serial stdio \
		-display none -no-reboot -m 128M 2>/dev/null; true
	@# Clean up the test-flavored kernel build. Next `make all` or
	@# `make docker-build` will produce a fresh runnable kernel.
	rm -rf build

DOCKER_RUN      = docker run --rm -u $$(id -u):$$(id -g) -v "$$(pwd)":/src lighthos-toolchain
DOCKER_RUN_ROOT = docker run --rm -v "$$(pwd)":/src lighthos-toolchain

docker-build:
	$(DOCKER_RUN) make all

docker-lua-compile:
	$(DOCKER_RUN) make lua-compile

# ---- Disk-bootable image (MBR + stage2 + kernel + simplefs partition) ----
#
# Layout of bootdisk.img:
#   LBA 0        : mbr.bin        (512 bytes)
#   LBA 1..62    : stage2.bin     (up to 31 KB, zero-padded)
#   LBA 63..2047 : kernel ELF     (raw bytes of build/lighthos.bin)
#   LBA 2048..   : simplefs partition (copied verbatim from disk.img)
#
# Producing bootdisk.img requires a pre-populated disk.img — boot once with
# `make run-disk` from the ISO, run `install` in the shell to copy user
# binaries into /disk/bin, and shut down. Then `make bootdisk` folds the
# result together with the bootloader and kernel into a self-contained
# bootable image.

build/mbr.bin: src/boot/disk/mbr.s
	@mkdir -p $(dir $@)
	$(AS) -f bin -o $@ $<

build/stage2.bin: src/boot/disk/stage2.s
	@mkdir -p $(dir $@)
	$(AS) -f bin -o $@ $<

# bootdisk.img is stamped together from disk.img + bootloader + kernel.
# Always rebuild on request: disk.img gets mutated in place by the ISO
# install flow, so make's mtime comparisons can't reliably tell when
# there's new content to restamp.
bootdisk: build/mbr.bin build/stage2.bin $(KERNEL_BIN) $(DISK_IMG)
	@mkdir -p build
	@echo "Building bootdisk.img from $(DISK_IMG) + MBR + stage2 + kernel"
	cp $(DISK_IMG) build/bootdisk.img
	dd if=build/mbr.bin of=build/bootdisk.img bs=512 count=1 conv=notrunc status=none
	dd if=build/stage2.bin of=build/bootdisk.img bs=512 seek=1 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=build/bootdisk.img bs=512 seek=63 conv=notrunc status=none
	@echo "bootdisk.img ready — boot with 'make run-bootdisk'"

# Kernel-only bootdisk: skip the FAT partition entirely. Useful for
# L6 smoke-testing the MBR + stage2 + ELF64 loader without the full
# user/ build. Produces a 1 MiB image: LBA 0 = MBR, LBA 1..62 =
# stage2, LBA 63..2047 = kernel ELF, rest zero.
bootdisk-bare: build/mbr.bin build/stage2.bin $(KERNEL_BIN)
	@mkdir -p build
	dd if=/dev/zero of=build/bootdisk-bare.img bs=1M count=1 status=none
	dd if=build/mbr.bin    of=build/bootdisk-bare.img bs=512 seek=0  conv=notrunc status=none
	dd if=build/stage2.bin of=build/bootdisk-bare.img bs=512 seek=1  conv=notrunc status=none
	dd if=$(KERNEL_BIN)    of=build/bootdisk-bare.img bs=512 seek=63 conv=notrunc status=none
	@echo "bootdisk-bare.img ready"

docker-bootdisk-bare:
	$(DOCKER_RUN) make bootdisk-bare

run-bootdisk-bare: docker-bootdisk-bare
	qemu-system-x86_64 -drive file=build/bootdisk-bare.img,format=raw,if=ide \
	    -display none -serial file:/tmp/bootdisk.log -m 128M -no-reboot \
	    -device isa-debug-exit,iobase=0x604,iosize=0x04 \
	    || true
	@echo "--- serial log ---"; cat /tmp/bootdisk.log

build/bootdisk.img: bootdisk

docker-bootdisk:
	$(DOCKER_RUN) make bootdisk

# Boot the disk image directly (no CDROM, no multiboot modules). If it
# works, LighthOS is self-hosting for the install + boot flow.
#
# Skips the bootdisk rebuild when build/bootdisk.img already exists —
# the whole point of run-bootdisk is to test file persistence across
# reboots, so regenerating the image from scratch every time defeats
# the purpose. `make bootdisk` or `rm build/bootdisk.img` forces a
# fresh one. `run-installed` is the "clean slate" entry point.
run-bootdisk:
	@if [ ! -f build/bootdisk.img ]; then \
	    echo "build/bootdisk.img missing — building a fresh one"; \
	    $(MAKE) docker-bootdisk; \
	else \
	    echo "using existing build/bootdisk.img (rm it to force a rebuild)"; \
	fi
	qemu-system-x86_64 -drive file=build/bootdisk.img,format=raw,if=ide \
		-nographic -m 128M

# Full "boot from installed drive" smoke test: build everything in
# docker, create FAT32 disk via mcopy, stamp bootloader + kernel,
# boot with disk as the ONLY media.
run-installed: docker-build docker-disk docker-bootdisk
	qemu-system-x86_64 -drive file=build/bootdisk.img,format=raw,if=ide \
		-nographic -m 128M

docker-test:
	$(DOCKER_RUN) make test

# ---- In-system test harness -----------------------------------------
# Builds build/lighthos-test.iso: the kernel + a grub.cfg entry that
# sets autorun=/bin/runtests on the multiboot cmdline + each ported
# test binary as a module. Kernel spawns /bin/runtests, which fork+
# execve's each test, tallies pass/fail, prints the summary line the
# host harness greps for, and calls sys_shutdown.

TEST_VSH = $(wildcard tests/*.vsh)

build/lighthos-test.iso: $(KERNEL_BIN) grub-test.cfg x64-userland \
                         $(X64_USER_EXTRA) \
                         build/sysroot/usr/lib/libtestdl.so.1 \
                         build/sysroot/usr/lib/libulib.so.1 \
                         build/sysroot/usr/lib/libvibc.so.1 \
                         build/sysroot/usr/lib/ld-lighthos.so.1 \
                         build/sysroot/usr/lib/luamod.so \
                         $(TEST_VSH)
	@rm -rf build/iso-test/boot
	@mkdir -p build/iso-test/boot/grub build/iso-test/boot/tests build/iso-test/boot/lib
	cp $(KERNEL_BIN) build/iso-test/boot/lighthos.bin
	cp grub-test.cfg build/iso-test/boot/grub/grub.cfg
	@for f in $(X64_USER_TARGETS) $(X64_USER_EXTRA) \
	          $(BUILD_USER)/dynhello $(BUILD_USER)/dyn_echo \
	          $(BUILD_USER)/dlopentest; do \
	    name=$$(basename $$f); \
	    cp $$f build/iso-test/boot/$$name; \
	done
	cp build/sysroot/usr/lib/libtestdl.so.1 build/iso-test/boot/lib/libtestdl.so.1
	cp build/sysroot/usr/lib/libulib.so.1   build/iso-test/boot/lib/libulib.so.1
	cp build/sysroot/usr/lib/libvibc.so.1   build/iso-test/boot/lib/libvibc.so.1
	cp build/sysroot/usr/lib/ld-lighthos.so.1 build/iso-test/boot/lib/ld-lighthos.so.1
	cp build/sysroot/usr/lib/luamod.so       build/iso-test/boot/lib/luamod.so
	@for f in $(TEST_VSH); do cp $$f build/iso-test/boot/tests/; done
	grub-mkrescue -o $@ build/iso-test 2>/dev/null

test-iso: build/lighthos-test.iso

docker-test-iso:
	$(DOCKER_RUN) make test-iso

# test-disk: boot the test ISO, let runtests walk /tests/*.vsh,
# capture serial, report PASS/FAIL tallies.
#
# A script "passes" when every `assert` in it returns PASS. Scripts
# gated on unported kernel features (signals, job control, mmap,
# alarm, strace, chroot, env, ld.so, lua) are currently expected to
# fail; they're tracked in PORT_EXPECTED_FAIL so the target exits
# non-zero only on an unexpected regression.
PORT_EXPECTED_FAIL =

test-disk:
	@if [ ! -f build/lighthos-test.iso ]; then \
	  echo "Missing build/lighthos-test.iso — run 'make docker-test-iso' first."; \
	  exit 1; \
	fi
	@if [ ! -f $(DISK_IMG) ]; then \
	  echo "Missing $(DISK_IMG) — run 'make docker-disk' first."; \
	  exit 1; \
	fi
	@mkdir -p build
	@timeout 90 qemu-system-x86_64 -cdrom build/lighthos-test.iso \
	      -drive file=$(DISK_IMG),format=raw,if=ide \
	      -display none -serial file:build/test-output.log -m 128M -no-reboot \
	      -device isa-debug-exit,iobase=0x604,iosize=0x04 \
	      </dev/null >/dev/null 2>&1; true
	@if grep -qE 'KERNEL PANIC|Exception:' build/test-output.log; then \
	  grep -E '^(=== |PASS |FAIL |Exception|KERNEL PANIC)' build/test-output.log; \
	  echo "KERNEL FAULT"; exit 1; \
	fi
	@echo "--- per-script results ---"
	@grep -E '^=== (OK|FAIL)' build/test-output.log || true
	@echo ""
	@echo "--- assertion tallies ---"
	@P=$$(grep -c '^PASS ' build/test-output.log 2>/dev/null); \
	 F=$$(grep -c '^FAIL ' build/test-output.log 2>/dev/null); \
	 echo "  $$P PASS, $$F FAIL"
	@# Gate: every script must either be in PORT_EXPECTED_FAIL or OK.
	@FAIL_UNEXPECTED=0; \
	 for f in $$(grep -E '^=== FAIL ' build/test-output.log | awk '{print $$3}'); do \
	   if ! echo " $(PORT_EXPECTED_FAIL) " | grep -q " $$f "; then \
	     echo "UNEXPECTED fail: $$f"; FAIL_UNEXPECTED=1; \
	   fi; \
	 done; \
	 if [ $$FAIL_UNEXPECTED -ne 0 ]; then \
	   echo "REGRESSION: see unexpected-fail list above"; exit 1; \
	 fi
	@echo ""
	@echo "All currently-achievable tests passed; scripts gated on"
	@echo "unported kernel features are in PORT_EXPECTED_FAIL."

docker-test-disk: docker-test-iso docker-disk
	$(MAKE) test-disk

docker-run: docker-build
	make run

# Fix ownership of any files created by past Docker-as-root runs.
# Uses a root Docker container (no -u flag) to chown everything to host user.
fix-perms:
	$(DOCKER_RUN_ROOT) chown -R $$(id -u):$$(id -g) /src
