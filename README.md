# LighthOS

A hobby **x86_64 long-mode** kernel, written in C (plus a little nasm),
with a real user-space ELF pipeline — shared libraries, a runtime
linker, FAT32 filesystem, job control, signals, a Lua interpreter with
working `require`, VGA graphics mode, and an in-system test harness
that drives both shell scripts and Lua assertions.

> **Status:** x86_64 / long-mode is the primary branch. The earlier
> i686 line is preserved only in history. 29 test scripts / 66
> assertions run green on `make docker-test-disk` in about 6 seconds.

## What's in the box

- **Kernel** — 4-level paging (PML4/PDPT/PD/PT) with higher-half
  kernel at `0xFFFFFFFF80000000` and HHDM at `0xFFFF800000000000`,
  per-process PML4, FXSAVE/FXRSTOR context switching, `SYSCALL`/`SYSRET`
  fast path, PMM + 32 MiB kernel heap, preemptive scheduler (100 Hz
  PIT) with `TASK_BLOCKED` / `TASK_STOPPED` states and `SYS_PAUSE` so
  idle tasks cost zero CPU, ATA driver, VFS with FAT16/FAT32 (r/w +
  LFN), ramfs, pipes, process groups + terminal foreground, signals
  (user-space handlers, Ctrl-C/Z/fg/bg, alarm), strace ring, chroot,
  mount/umount, mmap_anon, boot-log capture, ACPI shutdown.
- **User space** — shell with history, tab completion, `$VAR`
  expansion, word-level line editing (Ctrl-A/E/U/K, Ctrl-Left/Right),
  job control (bg/fg + `[stopped]` banners), pipes + `<`/`>`/`>>`
  redirection, `-c` mode for scripted calls. Full coreutils set
  (cat/grep/head/tail/wc/ls/cp/mv/touch/find/sleep/mkdir/rm/echo/env),
  ps / lsblk / free / install, vi clone, stty, pkill, strace,
  hexdump / pagemap / regions, and a mode-13h Flappy Bird.
- **Lua 5.4** — dynamically linked against libvibc / libulib; full
  `require` with **pure-Lua AND C modules** via a POSIX `<dlfcn.h>`
  shim over ld.so's ops table. `io.popen` / `os.execute` route
  through `/bin/shell -c` so shell syntax works inside Lua scripts.
- **Dynamic linking** — kernel honors `PT_INTERP`; user-space
  `ld-lighthos.so.1` loads `DT_NEEDED` libraries at first-fit slots
  from `0x30000000`, applies RELATIVE / 64 / PC32 / PLT32 / GLOB_DAT /
  JUMP_SLOT / COPY relocations via `RELA`, and publishes
  `dlopen`/`dlsym`/`dlclose`/`dlerror` through a function table at
  `0x50000000`.
- **Graphics** — VGA mode 13h (320x200x256) with a user-mappable
  linear framebuffer at `0xA0000`. Text mode round-trip works: the
  kernel caches the character-generator plane at boot and restores it
  after graphics mode, so `flappy` can drop you back to a clean `$`
  prompt.
- **TTY** — cooked/raw line discipline (`SYS_TTY_RAW`), CSI-6n
  window-size probe with kernel-cached `(rows, cols)` exposed via
  `SYS_TTY_WINSZ` and `$LINES` / `$COLUMNS`, non-blocking input peek
  (`SYS_TTY_POLL`), `sys_tty_lastsrc` so apps can detect whether they
  were launched from serial or PS/2, PS/2 keyboard + VGA as a real
  interactive console on `run-vga`.
- **Sysroot** — headers + archives staged under
  `build/sysroot/usr/{include,lib}`. Static **and** shared versions
  of libvibc + libulib.
- **Tier-3 test harness** — runtests walks `/tests/` and dispatches
  `.vsh` files to `/bin/shell` and `.lua` files to `/bin/lua`. Lua
  tests use `tests/lua/testlib.lua` for PASS/FAIL assertions. ACPI
  shutdown on completion, no stdin timing hacks. 6-second CI run.

## Quick start

You need Docker (the toolchain is hermetic — `x86_64-elf-gcc` 14.2 +
binutils 2.43 built inside the container) and QEMU.

```sh
# Build + test (takes ~60s first time, ~6s for the test run itself)
make docker-test-disk
```

That runs **66 assertions across 29 test scripts**: FAT r/w, pipes,
signals, alarms, environment variables, dynamic linking (Lua `require`
of both pure-Lua and C modules), chroot, jobs, mount, strace, mmap,
VGA winsize, pkill, and more.

## Common targets

```sh
make docker-build       # kernel + user space + ISO
make docker-disk        # build/disk.img (FAT32, populated via mcopy)
make docker-bootdisk    # self-contained bootdisk.img (MBR + kernel + fs)
make run                # boot the ISO (serial only, -nographic)
make run-disk           # boot ISO with disk.img attached
make run-bootdisk       # boot the self-contained disk (reuses img if present)
make run-installed      # end-to-end: build everything + boot fresh
make run-vga            # boot bootdisk with a QEMU VGA window
make run-vga-iso        # same but from the ISO
make run-vga-both       # VGA window AND interactive serial on stdio
make run-gdb            # QEMU -s -S; attach x86_64-elf-gdb on :1234
make clean              # rm -rf build/
```

Interactive QEMU console (`-nographic`): `Ctrl-A X` to quit. VGA
window: close the window, or `q` inside `flappy` drops back to text.

`run-bootdisk` (and the `run-vga*` targets) **reuse the existing
`build/bootdisk.img`** if one is present so the FAT32 partition keeps
any files you wrote across reboots. `rm build/bootdisk.img` or
`make run-installed` forces a clean image.

## Layout

```
src/kernel/    core kernel (process, task, syscall, elf, panic, isr)
src/mm/        pmm, vmm (per-process PML4, kernel half shared)
src/fs/        vfs + ramfs + fat + fstab
src/drivers/   serial, vga (text + mode-13h), ata, console, keyboard
src/lib/       kprintf (with boot-log capture), string
src/boot/      MBR + stage2 bootloader
user/          user-space sources (shell, coreutils, vi, flappy, …)
user/libc/     libvibc — stdio, malloc, snprintf, strtod, setjmp,
               ctype, errno, locale, misc, dlfcn shim
user/ldso/     the dynamic linker (ld-lighthos.so.1)
user/libluamod/ example Lua C module
tests/*.vsh    shell-driven tests
tests/*.lua    Lua-driven tests (via tests/lua/testlib.lua)
build/         all artifacts (gitignored)
```

## Constraints you'll notice

- Kernel runs at ring 0 with SSE disabled (`-mno-sse`); user code gets
  SSE via `fxsave`/`fxrstor` on context switch.
- User-mode main executables are non-PIE at `0x400000`. Shared
  libraries load first-fit from `0x30000000`; ld.so lives at
  `0x40000000`, its dlopen iface at `0x50000000`, the libvibc malloc
  arena starts at `0x60000000`, a mode-13h framebuffer alias at
  `0x70000000`, and user stack tops at `0x800000`.
- VGA text mode is hardware-fixed at 80x25. Graphics mode is 320x200.
  Serial uses whatever the host terminal is; the shell probes via
  CSI-6n at interactive startup and exports `LINES`/`COLUMNS`.
- FAT32 is the primary installed filesystem. No ext2 (yet).
- `PROCESS_MAX` / `TASK_MAX` are both 64.

## License

Apache 2.0 — see [`LICENSE`](LICENSE).
