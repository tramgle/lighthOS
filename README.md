# LighthOS

A hobby i686 kernel, written in C (plus a little nasm), with a real
user-space ELF pipeline — shared libraries, a runtime linker, FAT32
filesystem, job control, signals, and an in-system test harness.

> **Status:** 32-bit (i686) is functional and tested. 64-bit port is
> the next major milestone. See `AGENTS.md` for detailed state.

## What's in the box

- **Kernel** — paging with per-process page directories, PMM + heap,
  cooperative + preemptive task scheduling, ATA driver, VFS with FAT16/
  FAT32 (r/w, LFN), ramfs, pipes, process groups, signals (user-space
  handlers + alarm), strace, mount/umount, panic-log-to-FAT, serial
  GDB stub on COM2.
- **User space** — ~30 binaries including a shell with job control,
  vi clone, Lua 5.4 (as a dynamic executable), cat/grep/sed-style
  utilities, and a test harness (`runtests`).
- **Dynamic linking** — kernel honors `PT_INTERP`; user-space
  `ld-lighthos.so.1` loads `DT_NEEDED` libraries, applies
  relocations (R_386_RELATIVE / GLOB_DAT / JMP_SLOT / COPY / 32 /
  PC32), supports `dlopen`/`dlsym`.
- **Sysroot** — headers + archives staged under
  `build/sysroot/usr/{include,lib}`.
- **Tier-3 test harness** — `autorun=/tests` on the kernel cmdline
  runs the full test suite, ACPI-shuts-down on exit, no stdin
  timing hacks. One `make` away from a 6-second CI run.

## Quick start

You need Docker (toolchain is hermetic) and QEMU.

```sh
# Build + test (takes ~60s first time, ~6s for the test run itself)
make docker-test-disk
```

That runs 40 assertions across 18 test scripts (FAT, pipes, signals,
alarms, environment variables, dynamic linking, chroot, jobs, mount,
strace, mmap, and more).

## Common targets

```sh
make docker-build       # kernel + user space + ISO
make docker-disk        # build/disk.img (FAT32, populated via mcopy)
make docker-bootdisk    # self-contained bootdisk.img (MBR + kernel + fs)
make run                # boot the ISO
make run-disk           # boot ISO with disk.img attached
make run-bootdisk       # boot the self-contained disk
make run-installed      # end-to-end: build everything + run the bootdisk
make run-gdb            # boot with kernel gdb stub on tcp::1234
make clean              # rm -rf build/
```

Interactive QEMU console: `Ctrl-A X` to quit.

## Layout

```
src/kernel/    core kernel (process, syscall, elf, task, gdbstub, panic)
src/mm/        pmm, vmm (kernel PDEs 0-3 shared, 4-1023 private)
src/fs/        vfs + ramfs + fat + fstab
src/drivers/   serial, vga, ata, console, keyboard
src/lib/       kprintf (with boot-log capture)
user/          user-space sources (no build artifacts)
user/libc/     subset libc (malloc, stdio, strtod, setjmp, …)
user/ldso/     the dynamic linker
tests/*.vsh    shell-driven tests
build/         all artifacts (gitignored)
```

See [`AGENTS.md`](AGENTS.md) for architectural decisions, gotchas,
and contributor notes — read that file before making structural
changes.

## Constraints you'll notice

- i686 only (for now). x86_64 port is planned.
- Everything kernel-touchable lives in the first 16 MB of physical
  RAM — page tables, page directories, user frames. Enforced by
  `check_low()` in `src/mm/vmm.c`.
- Main executables are non-PIE at `0x08048000`. Shared libraries are
  PIC and load at `0x30000000+`. Interpreter is a static ET_EXEC at
  `0x40000000`.
- FAT32 is the primary installed filesystem. No ext2 (yet).

## License

Apache 2.0 — see [`LICENSE`](LICENSE).
