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
    src/kernel/gdt.c \
    src/kernel/tss.c \
    src/kernel/idt.c \
    src/kernel/isr.c \
    src/kernel/pic.c \
    src/kernel/elf.c \
    src/kernel/task.c \
    src/kernel/process.c \
    src/kernel/syscall.c \
    src/kernel/panic.c \
    src/kernel/debug.c \
    src/kernel/timer.c \
    src/mm/pmm.c \
    src/mm/vmm.c \
    src/mm/heap.c \
    src/lib/string.c \
    src/lib/kprintf.c \
    src/drivers/vga.c \
    src/drivers/serial.c \
    src/fs/vfs.c \
    src/fs/ramfs.c \
    src/fs/blkdev.c \
    src/fs/fat.c \
    src/fs/fstab.c \
    src/drivers/ata.c
  S_SOURCES := \
    src/boot/boot.s \
    src/kernel/gdt_flush.s \
    src/kernel/tss_flush.s \
    src/kernel/idt_flush.s \
    src/kernel/isr_stub.s \
    src/kernel/usermode.s
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

# x86_64-ported user binaries. Grows each time a user program is
# brought back; replaces the legacy SIMPLE_USER set during the port.
X64_USER        = hello forktest fstest
X64_USER_TARGETS = $(addprefix $(BUILD_USER)/,$(X64_USER))

X64_USER_CFLAGS = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -nostdlib \
                  -mno-red-zone

$(BUILD_USER)/crt0.o: user/crt0.s
	@mkdir -p $(dir $@)
	nasm -f elf64 $< -o $@

$(BUILD_USER)/%.o: user/%.c user/syscall_x64.h
	@mkdir -p $(dir $@)
	$(CC) $(X64_USER_CFLAGS) -c $< -o $@

$(X64_USER_TARGETS): $(BUILD_USER)/%: $(BUILD_USER)/crt0.o $(BUILD_USER)/%.o
	@mkdir -p $(dir $@)
	x86_64-elf-ld -T user/user.ld -nostdlib -o $@ $^

.PHONY: x64-userland
x64-userland: $(X64_USER_TARGETS)

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

$(ISO): $(KERNEL_BIN) grub.cfg x64-userland
	@mkdir -p build/iso/boot/grub
	cp $(KERNEL_BIN) build/iso/boot/lighthos.bin
	cp grub.cfg build/iso/boot/grub/grub.cfg
	@# Stage each ported user binary into the ISO so grub.cfg's
	@# `module /boot/<name> /bin/<name>` lines can drop them into
	@# ramfs at the declared paths.
	@for f in $(X64_USER_TARGETS); do \
	    name=$$(basename $$f); \
	    if [ -x $$f ]; then cp $$f build/iso/boot/$$name; fi; \
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

$(DISK_IMG): x64-userland
	@mkdir -p $(dir $@)
	@echo "Creating $(DISK_SIZE_MB) MB disk with FAT32 at LBA $(DISK_FAT_OFFSET)"
	dd if=/dev/zero of=$@ bs=1M count=$(DISK_SIZE_MB) 2>/dev/null
	mkfs.fat -F 32 -n LIGHTHOS --offset=$(DISK_FAT_OFFSET) $@ >/dev/null 2>&1
	mmd -i $@@@$$(($(DISK_FAT_OFFSET)*512)) ::BIN 2>/dev/null || true
	@for f in $(X64_USER_TARGETS); do \
	    name=$$(basename $$f); \
	    mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o $$f ::BIN/$$name; \
	done
	@# Copy init: pick hello as default so bootdisk boots the smoke test.
	mcopy -i $@@@$$(($(DISK_FAT_OFFSET)*512)) -D o $(BUILD_USER)/hello ::BIN/init
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
