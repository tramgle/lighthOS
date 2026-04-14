TARGET  = i686-elf
CC      = $(TARGET)-gcc
AS      = nasm
LD      = $(TARGET)-gcc

DEBUG_KERNEL ?= 0
CFLAGS  = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Isrc -mno-sse -mno-mmx -mno-sse2 -DDEBUG_KERNEL=$(DEBUG_KERNEL)
ASFLAGS = -f elf32
LDFLAGS = -T linker.ld -nostdlib -lgcc -ffreestanding

C_SOURCES = $(shell find src -name '*.c')
# Exclude src/boot/disk/ — those are real-mode sources for the disk
# bootloader, assembled as flat binaries rather than linked into the
# kernel ELF.
S_SOURCES = $(shell find src -name '*.s' -not -path 'src/boot/disk/*')

C_OBJECTS = $(patsubst src/%.c, build/%.o, $(C_SOURCES))
S_OBJECTS = $(patsubst src/%.s, build/%.o, $(S_SOURCES))
OBJECTS   = $(C_OBJECTS) $(S_OBJECTS)

KERNEL_BIN = build/vibeos.bin
ISO        = build/vibeos.iso

.PHONY: all clean iso run run-disk run-vga debug docker-build docker-run test docker-test iso-ready user-programs fix-perms docker-lua-compile bootdisk run-bootdisk docker-bootdisk patch-disk refresh-bootdisk

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

$(ISO): $(KERNEL_BIN) grub.cfg user-programs
	@mkdir -p build/iso/boot/grub
	cp $(KERNEL_BIN) build/iso/boot/vibeos.bin
	cp grub.cfg build/iso/boot/grub/grub.cfg
	-cp user/hello build/iso/boot/hello 2>/dev/null
	-cp user/shell build/iso/boot/shell 2>/dev/null
	-cp user/vi build/iso/boot/vi 2>/dev/null
	-cp user/bomb build/iso/boot/bomb 2>/dev/null
	-cp user/fork_test build/iso/boot/fork_test 2>/dev/null
	-cp user/free build/iso/boot/free 2>/dev/null
	-cp user/regions build/iso/boot/regions 2>/dev/null
	-cp user/pagemap build/iso/boot/pagemap 2>/dev/null
	-cp user/hexdump build/iso/boot/hexdump 2>/dev/null
	-cp user/libc_test build/iso/boot/libc_test 2>/dev/null
	-cp user/lua build/iso/boot/lua 2>/dev/null
	-cp user/install build/iso/boot/install 2>/dev/null
	-cp user/lsblk build/iso/boot/lsblk 2>/dev/null
	-cp user/test_badptr build/iso/boot/test_badptr 2>/dev/null
	grub-mkrescue -o $@ build/iso 2>/dev/null

disk.img:
	dd if=/dev/zero of=disk.img bs=1M count=16
	@# Format the FAT partition (sectors 20480..32767 = 6 MB) with a
	@# few sample files so the kernel's /fat mount has something to
	@# show. `mkfs.fat` / `mcopy` come from dosfstools + mtools. If
	@# they aren't installed this step silently skips — the FAT mount
	@# will just be empty on boot.
	-@which mkfs.fat >/dev/null 2>&1 && $(MAKE) --no-print-directory fat-prep || \
		echo "(mkfs.fat not installed; /fat partition left blank)"

.PHONY: fat-prep
fat-prep: disk.img
	@echo "Seeding /fat partition at LBA 20480"
	@mkfs.fat -F 16 -n VIBEOS --offset=20480 disk.img 12288 >/dev/null 2>&1 || true
	@printf 'Hello from the host!\nThis file was written via mtools before VibeOS booted.\n' > /tmp/vibeos-fat-hello.txt
	@mcopy -i disk.img@@$$((20480*512)) /tmp/vibeos-fat-hello.txt ::HELLO.TXT 2>/dev/null || true
	@rm -f /tmp/vibeos-fat-hello.txt

# run targets check that ISO exists; build it via Docker if missing.
# -nographic routes all I/O (serial + monitor) to stdio. Ctrl-A X to quit.
run: iso-ready
	qemu-system-i386 -cdrom $(ISO) -nographic -m 128M

run-disk: iso-ready disk.img
	qemu-system-i386 -cdrom $(ISO) -drive file=disk.img,format=raw,if=ide \
		-nographic -m 128M

# run-vga opens a graphical window (or VNC) with VGA, serial on stdio
run-vga: iso-ready
	qemu-system-i386 -cdrom $(ISO) -serial stdio -m 128M

debug: iso-ready
	qemu-system-i386 -cdrom $(ISO) -nographic -m 128M \
		-d int,cpu_reset -no-reboot -no-shutdown

iso-ready:
	@if [ ! -f $(ISO) ]; then \
		echo "ISO missing, running docker-build..."; \
		$(MAKE) docker-build; \
	fi

USER_CFLAGS = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -nostdlib -Iuser -Iuser/libc/include -mno-sse -mno-mmx -mno-sse2
USER_LDFLAGS = -T user/user.ld -nostdlib -ffreestanding -lgcc

USER_LIBC_SRC = user/libc/malloc.c user/libc/mem.c user/libc/string_extra.c \
                user/libc/ctype.c user/libc/snprintf.c user/libc/strtod.c \
                user/libc/stdio.c user/libc/errno.c user/libc/locale.c \
                user/libc/misc.c
USER_LIBC_OBJS = $(USER_LIBC_SRC:.c=.o) user/libc/setjmp.o

# Lua sources (Lua 5.4.7, vendored). lua.c/luac.c/linit.c are excluded —
# we provide our own init and main.
LUA_SRC_DIR = third_party/lua/src
LUA_CORE = lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject \
           lopcodes lparser lstate lstring ltable ltm lundump lvm lzio
LUA_LIB  = lauxlib lbaselib lmathlib lstrlib ltablib liolib loslib lcorolib lutf8lib
LUA_OBJS = $(addprefix build/lua/, $(addsuffix .o, $(LUA_CORE) $(LUA_LIB)))

LUA_CFLAGS = $(USER_CFLAGS) -I$(LUA_SRC_DIR) -include user/luaconf_vibeos.h \
             -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare \
             -Wno-implicit-fallthrough -Wno-parentheses -Wno-empty-body \
             -Wno-maybe-uninitialized -Wno-unused-but-set-variable

user-programs: user/hello user/shell user/vi user/bomb user/fork_test \
               user/free user/regions user/pagemap user/hexdump user/libc_test \
               user/lua user/install user/lsblk user/test_badptr

user/crt0.o: user/crt0.s
	$(AS) $(ASFLAGS) $< -o $@

user/libc/setjmp.o: user/libc/setjmp.s
	$(AS) $(ASFLAGS) $< -o $@

user/libc/%.o: user/libc/%.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

build/lua/%.o: $(LUA_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

.PHONY: lua-compile
lua-compile: $(LUA_OBJS)
	@echo "Lua sources compiled: $(words $(LUA_OBJS)) objects"

user/ulib.o: user/ulib.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/%.o: user/%.c
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/hello: user/crt0.o user/hello.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

user/shell: user/crt0.o user/ulib.o user/shell.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

user/vi: user/crt0.o user/ulib.o user/vi.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

user/bomb: user/crt0.o user/ulib.o user/bomb.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

user/fork_test: user/crt0.o user/ulib.o user/fork_test.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

user/free: user/crt0.o user/ulib.o user/free.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

user/regions: user/crt0.o user/ulib.o user/regions.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

user/pagemap: user/crt0.o user/ulib.o user/pagemap.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

user/hexdump: user/crt0.o user/ulib.o user/hexdump.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

user/libc_test: user/crt0.o user/ulib.o $(USER_LIBC_OBJS) user/libc_test.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

build/lua/linit_vibeos.o: user/linit_vibeos.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

build/lua/lua_main.o: user/lua_main.c
	@mkdir -p $(dir $@)
	$(CC) $(LUA_CFLAGS) -c $< -o $@

user/lua: user/crt0.o user/ulib.o $(USER_LIBC_OBJS) $(LUA_OBJS) build/lua/linit_vibeos.o build/lua/lua_main.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

user/install: user/crt0.o user/ulib.o user/install.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

user/lsblk: user/crt0.o user/ulib.o user/lsblk.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

user/test_badptr: user/crt0.o user/ulib.o user/test_badptr.o
	$(CC) $(USER_LDFLAGS) -o $@ $^

clean:
	rm -rf build user/*.o user/libc/*.o user/hello user/shell user/vi user/bomb user/fork_test \
	       user/free user/regions user/pagemap user/hexdump user/libc_test user/lua user/install

test: clean
	$(MAKE) CFLAGS="$(CFLAGS) -DRUN_TESTS" $(ISO)
	timeout 10 qemu-system-i386 -cdrom $(ISO) -serial stdio \
		-display none -no-reboot -m 128M 2>/dev/null; true
	@# Clean up the test-flavored kernel build. Next `make all` or
	@# `make docker-build` will produce a fresh runnable kernel.
	rm -rf build

DOCKER_RUN      = docker run --rm -u $$(id -u):$$(id -g) -v "$$(pwd)":/src vibeos-toolchain
DOCKER_RUN_ROOT = docker run --rm -v "$$(pwd)":/src vibeos-toolchain

docker-build:
	$(DOCKER_RUN) make all

docker-lua-compile:
	$(DOCKER_RUN) make lua-compile

# ---- Disk-bootable image (MBR + stage2 + kernel + simplefs partition) ----
#
# Layout of bootdisk.img:
#   LBA 0        : mbr.bin        (512 bytes)
#   LBA 1..62    : stage2.bin     (up to 31 KB, zero-padded)
#   LBA 63..2047 : kernel ELF     (raw bytes of build/vibeos.bin)
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
bootdisk: build/mbr.bin build/stage2.bin $(KERNEL_BIN) disk.img
	@mkdir -p build
	@echo "Building bootdisk.img from disk.img + MBR + stage2 + kernel"
	cp disk.img build/bootdisk.img
	dd if=build/mbr.bin of=build/bootdisk.img bs=512 count=1 conv=notrunc status=none
	dd if=build/stage2.bin of=build/bootdisk.img bs=512 seek=1 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=build/bootdisk.img bs=512 seek=63 conv=notrunc status=none
	@echo "bootdisk.img ready — boot with 'make run-bootdisk'"

build/bootdisk.img: bootdisk

docker-bootdisk:
	$(DOCKER_RUN) make bootdisk

# Rapid iteration: rebuild user binaries + kernel in docker, then boot
# the ISO against the existing disk.img with an auto-run `install`.
# Fast alternative to the manual run-disk -> install -> shutdown cycle
# we'd otherwise do every time a binary changes.
patch-disk: docker-build disk.img
	@echo "Refreshing /disk/bin from latest build..."
	@(sleep 2; printf '/bin/install\n'; sleep 10; printf 'shutdown\n'; sleep 2) \
		| timeout 30 qemu-system-i386 -cdrom build/vibeos.iso \
			-drive file=disk.img,format=raw,if=ide \
			-nographic -m 128M > /dev/null 2>&1
	@echo "patch-disk done — disk.img updated"

# One-stop refresh: rebuild everything, reinstall user bins, rebuild
# bootdisk. After this, `make run-bootdisk` picks up all source changes.
refresh-bootdisk: patch-disk docker-bootdisk

# Boot the disk image directly (no CDROM, no multiboot modules). If it
# works, VibeOS is self-hosting for the install + boot flow.
run-bootdisk: build/bootdisk.img
	qemu-system-i386 -drive file=build/bootdisk.img,format=raw,if=ide \
		-nographic -m 128M

docker-test:
	$(DOCKER_RUN) make test

docker-run: docker-build
	make run

# Fix ownership of any files created by past Docker-as-root runs.
# Uses a root Docker container (no -u flag) to chown everything to host user.
fix-perms:
	$(DOCKER_RUN_ROOT) chown -R $$(id -u):$$(id -g) /src
