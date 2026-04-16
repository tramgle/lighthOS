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

# x86_64 port: during L1-L4 we progressively un-exclude files as
# they're ported. For now only boot.s + main.c (the L1 minimal
# kernel_main) link into the ELF; everything else still speaks
# 32-bit inline asm / register layouts.
# The variable PORT_MINIMAL defaults to 1 until the port is done.
PORT_MINIMAL ?= 1
ifeq ($(PORT_MINIMAL),1)
  # Sources that have been ported to x86_64 so far. Each milestone
  # grows this list. When the last file is ported, flip PORT_MINIMAL
  # back to 0 and delete src/port/shim.c.
  C_SOURCES := \
    src/kernel/main.c \
    src/mm/pmm.c \
    src/mm/vmm.c \
    src/lib/string.c \
    src/port/shim.c
  S_SOURCES := src/boot/boot.s
endif

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

.PHONY: all clean iso run run-disk run-vga debug docker-build docker-run test docker-test iso-ready user-programs fix-perms docker-lua-compile bootdisk run-bootdisk docker-bootdisk docker-disk test-iso docker-test-iso test-disk docker-test-disk run-installed

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

$(ISO): $(KERNEL_BIN) grub.cfg user-programs etc/fstab
	@mkdir -p build/iso/boot/grub build/iso/boot/etc
	cp $(KERNEL_BIN) build/iso/boot/lighthos.bin
	cp grub.cfg build/iso/boot/grub/grub.cfg
	cp etc/fstab build/iso/boot/etc/fstab
	@# Copy every built user binary into the ISO under /boot. Missing
	@# entries are skipped silently so a partial build still produces
	@# a bootable image. libc_test, lua, and dynhello aren't in
	@# SIMPLE_USER but are listed explicitly since they're built via
	@# their own rules.
	@for bin in $(SIMPLE_USER) libc_test lua dynhello dyn_echo; do \
		if [ -x $(BUILD_USER)/$$bin ]; then \
			cp $(BUILD_USER)/$$bin build/iso/boot/$$bin; \
		fi; \
	done
	@# Dynamic-linking runtime for test-ISO boots. ld.so + shared libs
	@# get staged as multiboot modules with /lib/ paths so the kernel
	@# module loader drops them at /lib/<name>.
	@mkdir -p build/iso/boot/lib
	@for so in ld-lighthos.so.1 libulib.so.1 libvibc.so.1 libtestdl.so.1; do \
		if [ -f $(SYSROOT_LIB)/$$so ]; then \
			cp $(SYSROOT_LIB)/$$so build/iso/boot/lib/$$so; \
		fi; \
	done
	grub-mkrescue -o $@ build/iso 2>/dev/null

# ---- Disk image with a single FAT32 partition at LBA 2048 ---------
# Layout: LBA 0=MBR, 1..62=stage2, 63..2047=kernel ELF, 2048+=FAT32.
# The partition is formatted + populated entirely from the host via
# mkfs.fat + mcopy — no QEMU round-trip needed. `make disk.img` is
# the one-stop entry point; `make patch-disk` is the legacy QEMU-based
# flow (still works for debugging but not needed for normal use).

DISK_SIZE_MB    = 64
DISK_FAT_OFFSET = 2048

$(DISK_IMG): user-programs
	@mkdir -p $(dir $@)
	@echo "Creating $(DISK_SIZE_MB) MB disk with FAT32 at LBA $(DISK_FAT_OFFSET)"
	dd if=/dev/zero of=$@ bs=1M count=$(DISK_SIZE_MB) 2>/dev/null
	mkfs.fat -F 32 -n LIGHTHOS --offset=$(DISK_FAT_OFFSET) $@ >/dev/null 2>&1
	mmd -i $@@@$$(($(DISK_FAT_OFFSET)*512)) ::BIN ::LIB 2>/dev/null || true
	@for f in $(SIMPLE_USER_TARGETS) $(BUILD_USER)/libc_test $(BUILD_USER)/lua \
	          $(BUILD_USER)/dynhello $(BUILD_USER)/dyn_echo; do \
		name=$$(basename $$f); \
		mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o $$f ::BIN/$$name; \
	done
	@# Dynamic-linking runtime: ld.so + shared libs.
	mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o \
	      $(SYSROOT_LIB)/ld-lighthos.so.1 ::LIB/ld-lighthos.so.1
	mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o \
	      $(SYSROOT_LIB)/libulib.so.1 ::LIB/libulib.so.1
	mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o \
	      $(SYSROOT_LIB)/libvibc.so.1 ::LIB/libvibc.so.1
	mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o \
	      $(SYSROOT_LIB)/libtestdl.so.1 ::LIB/libtestdl.so.1
	@echo "$@ ready ($(DISK_SIZE_MB) MB, FAT32)"
	@# No /etc/fstab on the disk: the kernel's built-in defaults
	@# mount this partition at '/' directly. A user-written /etc/fstab
	@# would be consulted here if we grew post-boot fstab rereading,
	@# but that's a future feature.

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

# run-vga opens a graphical window (or VNC) with VGA, serial on stdio
run-vga: iso-ready
	qemu-system-x86_64 -cdrom $(ISO) -serial stdio -m 128M

debug: iso-ready
	qemu-system-x86_64 -cdrom $(ISO) -nographic -m 128M \
		-d int,cpu_reset -no-reboot -no-shutdown

# run-gdb: boot the ISO with the kernel's gdb stub exposed on COM2
# as tcp::1234. Connect from another shell with:
#   i686-elf-gdb build/lighthos.bin -ex 'target remote localhost:1234'
# The kernel drops into the stub on any int3 (kernel or user).
# Insert breakpoints by adding `gdb_break();` in kernel source or via
# gdb's `break *0xADDR` (software int3 patched at runtime).
run-gdb: iso-ready $(DISK_IMG)
	qemu-system-x86_64 -cdrom $(ISO) -drive file=$(DISK_IMG),format=raw,if=ide \
		-m 128M -nographic \
		-serial mon:stdio \
		-serial tcp::1234,server,nowait \
		-no-reboot

iso-ready:
	@if [ ! -f $(ISO) ]; then \
		echo "ISO missing, running docker-build..."; \
		$(MAKE) docker-build; \
	fi

# Sysroot layout: user-space headers + archives live under
# build/sysroot/usr so the build mimics a conventional Unix staging
# tree. -I$(SYSROOT_INC) / -L$(SYSROOT_LIB) replace the old direct
# paths under user/. Once .so support lands, the runtime ld.so will
# also search $(SYSROOT_LIB).
SYSROOT     = build/sysroot
SYSROOT_INC = $(SYSROOT)/usr/include
SYSROOT_LIB = $(SYSROOT)/usr/lib
SYSROOT_BIN = $(SYSROOT)/usr/bin

USER_CFLAGS = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -nostdlib \
              -I$(SYSROOT_INC) \
              -mno-sse -mno-mmx -mno-sse2
USER_LDFLAGS = -T user/user.ld -nostdlib -ffreestanding -lgcc

# Stage headers into the sysroot. Copies are cheap and keep the
# source-of-truth in user/ and user/libc/include/. -MD would let us
# track transitive header deps but we're not there yet.
SYSROOT_HDRS_TOP  = ulib.h syscall.h luaconf_lighthos.h
SYSROOT_HDRS_LIBC = $(notdir $(wildcard user/libc/include/*.h))
SYSROOT_HDRS = $(addprefix $(SYSROOT_INC)/, $(SYSROOT_HDRS_TOP) $(SYSROOT_HDRS_LIBC))

$(SYSROOT_INC)/%.h: user/%.h
	@mkdir -p $(dir $@)
	@cp $< $@

$(SYSROOT_INC)/%.h: user/libc/include/%.h
	@mkdir -p $(dir $@)
	@cp $< $@

.PHONY: sysroot-headers
sysroot-headers: $(SYSROOT_HDRS)

USER_LIBC_SRC = user/libc/malloc.c user/libc/mem.c user/libc/string_extra.c \
                user/libc/ctype.c user/libc/snprintf.c user/libc/strtod.c \
                user/libc/stdio.c user/libc/errno.c user/libc/locale.c \
                user/libc/misc.c
USER_LIBC_OBJS = $(patsubst user/libc/%.c,$(BUILD_USER_LIBC)/%.o,$(USER_LIBC_SRC)) \
                 $(BUILD_USER_LIBC)/setjmp.o

# PIC variants for libvibc.so.1. setjmp.s is position-independent
# already (no absolute-address text references), so we reuse its
# object verbatim.
USER_LIBC_PIC_OBJS = $(patsubst user/libc/%.c,$(BUILD_USER_LIBC)/%.pic.o,$(USER_LIBC_SRC)) \
                     $(BUILD_USER_LIBC)/setjmp.o

# Lua sources (Lua 5.4.7, vendored). lua.c/luac.c/linit.c are excluded —
# we provide our own init and main.
LUA_SRC_DIR = third_party/lua/src
LUA_CORE = lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject \
           lopcodes lparser lstate lstring ltable ltm lundump lvm lzio
LUA_LIB  = lauxlib lbaselib lmathlib lstrlib ltablib liolib loslib lcorolib lutf8lib
LUA_OBJS = $(addprefix build/lua/, $(addsuffix .o, $(LUA_CORE) $(LUA_LIB)))

LUA_CFLAGS = $(USER_CFLAGS) -I$(LUA_SRC_DIR) -include $(SYSROOT_INC)/luaconf_lighthos.h \
             -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
             -Wno-implicit-fallthrough -Wno-parentheses -Wno-empty-body \
             -Wno-maybe-uninitialized -Wno-unused-but-set-variable

# Simple user binaries — crt0 + own .o + ulib.
#
# Split into two sets:
#   STATIC_USER  — the rescue set that must boot even if /lib/*.so.1
#                  is missing or the runtime linker is broken. Keep
#                  small. `install` populates /lib and /bin on a
#                  fresh disk; `runtests` drives the in-system test
#                  harness before dynamic libs may be installed;
#                  `hello` is the universal smoke test.
#   DYNAMIC_USER — everything else. Linked with PT_INTERP=/lib/
#                  ld-lighthos.so.1 and DT_NEEDED libulib.so.1 (and
#                  libvibc.so.1 where applicable).
#
# The full list is declared here (before user-programs uses it)
# because make expands variables in prereqs at parse time.
STATIC_USER  = hello install runtests echo
DYNAMIC_USER = shell vi bomb fork_test free regions pagemap hexdump \
               lsblk test_badptr cat cp mv touch head tail wc \
               grep find assert chroot ls sigtest sleep mount umount \
               alarmtest strace mmaptest env envtest dlopentest
SIMPLE_USER  = $(STATIC_USER) $(DYNAMIC_USER)
SIMPLE_USER_TARGETS  = $(addprefix $(BUILD_USER)/,$(SIMPLE_USER))
STATIC_USER_TARGETS  = $(addprefix $(BUILD_USER)/,$(STATIC_USER))
DYNAMIC_USER_TARGETS = $(addprefix $(BUILD_USER)/,$(DYNAMIC_USER))

user-programs: $(SIMPLE_USER_TARGETS) \
               $(BUILD_USER)/libc_test $(BUILD_USER)/lua \
               $(BUILD_USER)/dynhello $(BUILD_USER)/dyn_echo \
               $(SYSROOT_LIB)/ld-lighthos.so.1 \
               $(SYSROOT_LIB)/libulib.so.1 \
               $(SYSROOT_LIB)/libvibc.so.1 \
               $(SYSROOT_LIB)/libtestdl.so.1

$(BUILD_USER)/crt0.o: user/crt0.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_USER_LIBC)/setjmp.o: user/libc/setjmp.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_USER_LIBC)/%.o: user/libc/%.c $(SYSROOT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD_USER_LIBC)/%.pic.o: user/libc/%.c $(SYSROOT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS_PIC) -c $< -o $@

build/lua/%.o: $(LUA_SRC_DIR)/%.c $(SYSROOT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

.PHONY: lua-compile
lua-compile: $(LUA_OBJS)
	@echo "Lua sources compiled: $(words $(LUA_OBJS)) objects"

$(BUILD_USER)/ulib.o: user/ulib.c $(SYSROOT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# PIC variant of ulib for libulib.so.1 (shared object). Everything
# else that's static keeps using the regular $(BUILD_USER)/ulib.o.
USER_CFLAGS_PIC = $(USER_CFLAGS) -fPIC -fvisibility=default
$(BUILD_USER)/ulib.pic.o: user/ulib.c $(SYSROOT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS_PIC) -c $< -o $@

$(BUILD_USER)/%.o: user/%.c $(SYSROOT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# ---- Static library archives (sysroot-staged) ----------------------
# libulib: minimal syscall wrappers, strings, printf. Every user
# binary links it via `-lulib`.
# libvibc: subset libc (malloc, stdio FILE*, strtod, snprintf, setjmp,
# ctype). Binaries that need it link `-lvibc -lulib` — link order
# matters because libvibc calls into ulib for strlen/memcpy.
$(SYSROOT_LIB)/libulib.a: $(BUILD_USER)/ulib.o
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(SYSROOT_LIB)/libvibc.a: $(USER_LIBC_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# libulib.so.1: shared-object version of ulib. Loaded at an
# ld.so-chosen base (first-fit from 0x30000000). `-Wl,-shared` forces
# the flag through to ld (gcc's i686-elf freestanding spec swallows a
# bare `-shared`), producing ET_DYN. `-soname` records the run-time
# name so main executables that list this lib in DT_NEEDED find it
# by soname.
$(SYSROOT_LIB)/libulib.so.1: $(BUILD_USER)/ulib.pic.o
	@mkdir -p $(dir $@)
	$(CC) -nostdlib -ffreestanding \
	      -Wl,-shared -Wl,-soname,libulib.so.1 \
	      -Wl,-z,max-page-size=0x1000 \
	      -o $@ $(BUILD_USER)/ulib.pic.o -lgcc
	@# Convenience symlink so `-lulib` picks up the shared form by
	@# default. Removing this would force `-l:libulib.so.1` at link time.
	@ln -sf libulib.so.1 $(SYSROOT_LIB)/libulib.so

# libtestdl.so.1: smallest-possible shared object — two trivial
# function exports, no libulib dependency. Used by /bin/dlopentest to
# exercise the ld-lighthos.so.1 dlopen/dlsym path.
$(BUILD_USER)/libtestdl/libtestdl.pic.o: user/libtestdl/libtestdl.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS_PIC) -c $< -o $@

$(SYSROOT_LIB)/libtestdl.so.1: $(BUILD_USER)/libtestdl/libtestdl.pic.o
	@mkdir -p $(dir $@)
	$(CC) -nostdlib -ffreestanding \
	      -Wl,-shared -Wl,-soname,libtestdl.so.1 \
	      -Wl,-z,max-page-size=0x1000 \
	      -o $@ $< -lgcc

# libvibc.so.1: shared-object form of the subset libc. Declares a
# run-time dependency on libulib.so.1 (for strlen/memcpy/sys_*).
# The -L / -lulib bits aren't consumed for symbol copy-in (it's a
# shared library, not a final link), but they plant the DT_NEEDED
# entry via ld's normal handling.
$(SYSROOT_LIB)/libvibc.so.1: $(USER_LIBC_PIC_OBJS) $(SYSROOT_LIB)/libulib.so.1
	@mkdir -p $(dir $@)
	$(CC) -nostdlib -ffreestanding \
	      -Wl,-shared -Wl,-soname,libvibc.so.1 \
	      -Wl,-z,max-page-size=0x1000 \
	      -o $@ $(USER_LIBC_PIC_OBJS) \
	      -L$(SYSROOT_LIB) -lulib -lgcc
	@ln -sf libvibc.so.1 $(SYSROOT_LIB)/libvibc.so

# ---- ld-lighthos.so.1: the user-space dynamic linker ---------------
# Built as a plain ET_EXEC placed at 0x40000000 (see user/ldso/ldso.ld),
# statically linked against libulib.a so the interpreter has its own
# strcmp/puts/printf without any bootstrap problem. The kernel loads
# this alongside any main exec that declares PT_INTERP=/lib/ld-lighthos.so.1.
$(BUILD_USER_LDSO)/crt0_ldso.o: user/ldso/crt0_ldso.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_USER_LDSO)/ld_main.o: user/ldso/ld_main.c $(SYSROOT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(SYSROOT_LIB)/ld-lighthos.so.1: $(BUILD_USER_LDSO)/crt0_ldso.o \
                               $(BUILD_USER_LDSO)/ld_main.o \
                               user/ldso/ldso.ld $(SYSROOT_LIB)/libulib.a
	@mkdir -p $(dir $@)
	$(CC) -T user/ldso/ldso.ld -nostdlib -ffreestanding -static \
	      -Wl,--no-dynamic-linker -Wl,--build-id=none \
	      -o $@ $(BUILD_USER_LDSO)/crt0_ldso.o $(BUILD_USER_LDSO)/ld_main.o \
	      -Wl,-Bstatic -L$(SYSROOT_LIB) -lulib -lgcc \
	      -Wl,-Bdynamic

# Static rescue binaries: fully self-contained, no dynamic linker
# needed. `-Wl,-Bstatic` forces archive lookup over the .so symlinks
# we dropped next to them.
$(STATIC_USER_TARGETS): $(BUILD_USER)/%: $(BUILD_USER)/crt0.o $(BUILD_USER)/%.o $(SYSROOT_LIB)/libulib.a
	@mkdir -p $(dir $@)
	$(CC) $(USER_LDFLAGS) -o $@ $(BUILD_USER)/crt0.o $(BUILD_USER)/$*.o \
	      -Wl,-Bstatic -L$(SYSROOT_LIB) -lulib -Wl,-Bdynamic

# Dynamic binaries: main exec at 0x08048000, PT_INTERP=/lib/ld-lighthos.so.1,
# DT_NEEDED libulib.so.1. Symbols resolved at load time by the
# runtime linker.
$(DYNAMIC_USER_TARGETS): $(BUILD_USER)/%: $(BUILD_USER)/crt0.o $(BUILD_USER)/%.o $(SYSROOT_LIB)/libulib.so.1
	@mkdir -p $(dir $@)
	$(CC) -T user/user.ld -nostdlib -ffreestanding \
	      -Wl,--dynamic-linker=/lib/ld-lighthos.so.1 \
	      -Wl,--no-as-needed \
	      -Wl,-rpath-link,$(SYSROOT_LIB) \
	      -o $@ $(BUILD_USER)/crt0.o $(BUILD_USER)/$*.o \
	      -L$(SYSROOT_LIB) -lulib -lgcc

# Binaries that need libvibc (libc_test + lua) — now dynamic too.
# Resolving symbols through ld-lighthos.so.1 at load time means lua's
# 226 KB of libvibc + libulib duplication goes away in favor of
# a shared copy in /lib/.
$(BUILD_USER)/libc_test: $(BUILD_USER)/crt0.o $(BUILD_USER)/libc_test.o \
                         $(SYSROOT_LIB)/libvibc.so.1 $(SYSROOT_LIB)/libulib.so.1
	@mkdir -p $(dir $@)
	$(CC) -T user/user.ld -nostdlib -ffreestanding \
	      -Wl,--dynamic-linker=/lib/ld-lighthos.so.1 \
	      -Wl,--no-as-needed \
	      -Wl,-rpath-link,$(SYSROOT_LIB) \
	      -o $@ $(BUILD_USER)/crt0.o $(BUILD_USER)/libc_test.o \
	      -L$(SYSROOT_LIB) -lvibc -lulib -lgcc

build/lua/linit_lighthos.o: user/linit_lighthos.c $(SYSROOT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

build/lua/lua_main.o: user/lua_main.c $(SYSROOT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

$(BUILD_USER)/lua: $(BUILD_USER)/crt0.o $(LUA_OBJS) \
                   build/lua/linit_lighthos.o build/lua/lua_main.o \
                   $(SYSROOT_LIB)/libvibc.so.1 $(SYSROOT_LIB)/libulib.so.1
	@mkdir -p $(dir $@)
	$(CC) -T user/user.ld -nostdlib -ffreestanding \
	      -Wl,--dynamic-linker=/lib/ld-lighthos.so.1 \
	      -Wl,--no-as-needed \
	      -Wl,-rpath-link,$(SYSROOT_LIB) \
	      -o $@ $(BUILD_USER)/crt0.o $(LUA_OBJS) \
	      build/lua/linit_lighthos.o build/lua/lua_main.o \
	      -L$(SYSROOT_LIB) -lvibc -lulib -lgcc

# Dynamic binaries: link against libulib.so.1, record PT_INTERP, and
# let ld-lighthos.so.1 resolve symbols at load time. Keeps the same
# 0x08048000 text base as static binaries (non-PIE main exec), so
# user.ld still applies. `-Wl,--no-as-needed` forces libulib.so.1
# to stay in DT_NEEDED even if no direct symbol reference is visible
# at link time (important while we're piloting the flow).
$(BUILD_USER)/dynhello: $(BUILD_USER)/crt0.o $(BUILD_USER)/dynhello.o \
                        $(SYSROOT_LIB)/libulib.so.1
	@mkdir -p $(dir $@)
	$(CC) -T user/user.ld -nostdlib -ffreestanding \
	      -Wl,--dynamic-linker=/lib/ld-lighthos.so.1 \
	      -Wl,--no-as-needed \
	      -Wl,-rpath-link,$(SYSROOT_LIB) \
	      -o $@ $(BUILD_USER)/crt0.o $(BUILD_USER)/dynhello.o \
	      -L$(SYSROOT_LIB) -lulib -lgcc

$(BUILD_USER)/dyn_echo: $(BUILD_USER)/crt0.o $(BUILD_USER)/dyn_echo.o \
                        $(SYSROOT_LIB)/libvibc.so.1 \
                        $(SYSROOT_LIB)/libulib.so.1
	@mkdir -p $(dir $@)
	$(CC) -T user/user.ld -nostdlib -ffreestanding \
	      -Wl,--dynamic-linker=/lib/ld-lighthos.so.1 \
	      -Wl,--no-as-needed \
	      -Wl,-rpath-link,$(SYSROOT_LIB) \
	      -o $@ $(BUILD_USER)/crt0.o $(BUILD_USER)/dyn_echo.o \
	      -L$(SYSROOT_LIB) -lvibc -lulib -lgcc

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

build/bootdisk.img: bootdisk

docker-bootdisk:
	$(DOCKER_RUN) make bootdisk

# Boot the disk image directly (no CDROM, no multiboot modules). If it
# works, LighthOS is self-hosting for the install + boot flow.
run-bootdisk: docker-bootdisk
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
# test-iso builds build/lighthos-test.iso: production user binaries
# PLUS each tests/*.vsh as a multiboot module whose cmdline contains
# "tests/<name>.vsh". Kernel's module loader drops those into
# /tests/<name>.vsh in ramfs (see src/kernel/main.c), so the
# in-system `runtests /tests` walks them.

TEST_VSH = $(wildcard tests/*.vsh)

build/lighthos-test.iso: $(ISO) grub-test.cfg $(TEST_VSH)
	@mkdir -p build/iso-test/boot/grub build/iso-test/boot/tests
	@cp -r build/iso/boot/. build/iso-test/boot/
	@cp grub-test.cfg build/iso-test/boot/grub/grub.cfg
	@for f in $(TEST_VSH); do cp $$f build/iso-test/boot/tests/; done
	grub-mkrescue -o $@ build/iso-test 2>/dev/null

test-iso: build/lighthos-test.iso

docker-test-iso:
	$(DOCKER_RUN) make test-iso

# test-disk boots the test ISO, sends a single "runtests /tests;
# shutdown" line so there's no in-band stdin timing dependency, and
# checks the captured serial log for FAIL markers. Run
# `make docker-test-iso` first to build build/lighthos-test.iso in docker
# (host typically doesn't have grub-mkrescue).
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
	@# Tier-3 harness: the kernel sees `autorun=/tests` on its
	@# cmdline (set in grub-test.cfg) and spawns /bin/runtests itself,
	@# then ACPI-shuts-down on exit. No stdin dance, no sleep-45
	@# timing assumption. Redirect stdin from /dev/null so nothing
	@# can keep qemu alive.
	@timeout 90 qemu-system-x86_64 -cdrom build/lighthos-test.iso \
	      -drive file=$(DISK_IMG),format=raw,if=ide \
	      -nographic -m 128M </dev/null \
	  | tee build/test-output.log >/dev/null
	@# Fail if any FAIL lines appear, the summary is missing
	@# (autorun didn't finish), or a kernel panic fired.
	@if grep -qE '^FAIL|, [1-9][0-9]* FAIL|KERNEL PANIC' build/test-output.log \
	   || ! grep -qE '=== Summary:' build/test-output.log; then \
	  grep -E '^(=== |PASS|FAIL|Exception|KERNEL PANIC)' build/test-output.log; \
	  echo "TEST FAILURES"; exit 1; \
	else \
	  grep -E '^(=== |PASS|FAIL)' build/test-output.log; \
	  echo "ALL TESTS PASSED"; \
	fi

# One-stop test run: rebuild everything in docker, then boot on host.
docker-test-disk: docker-test-iso docker-disk
	$(MAKE) test-disk

docker-run: docker-build
	make run

# Fix ownership of any files created by past Docker-as-root runs.
# Uses a root Docker container (no -u flag) to chown everything to host user.
fix-perms:
	$(DOCKER_RUN_ROOT) chown -R $$(id -u):$$(id -g) /src
