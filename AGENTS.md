# LighthOS — agent launch point

Quick orientation doc for future sessions. Read this first; pair
with the long-form memory snapshot in Claude Code's auto-memory
(path derived from the repo's absolute location — if still at
`~/projects/vibeos`, it's
`.claude/projects/-home-stolley-projects-vibeos/memory/project_vibeos.md`).

## What this is

Hobby **x86_64** kernel in C + nasm. The i686 port has been
ported to long mode. Branch `x64-port` holds the full port work;
it has not been merged into `main`.

As of HEAD on `x64-port`:

- **Kernel**: 4-level paging (PML4/PDPT/PD/PT), higher-half at
  `0xFFFFFFFF80000000` + HHDM at `0xFFFF800000000000`, per-process
  PML4 (kernel PML4[256..511] shared, user PML4[0] private),
  heap at `0xFFFFFFFF81000000` (32 MiB). Tasks with FXSAVE/FXRSTOR
  context switching. VFS, FAT32 r/w + LFN, ATA, pipes, signals
  (user handlers, SIGINT/SIGALRM/SIGSTOP/SIGCONT routing), pgroup
  / fg tracking, strace ring, mount/umount, chroot, envp/auxv.
- **Syscall entry**: `syscall` instruction path via MSRs
  (STAR/LSTAR/SFMASK). INT 0x80 kept registered as a
  debuggable fallback but nothing uses it.
- **Serial line-discipline**: kernel-side echo for printable +
  Enter + BS; line_len guard stops BS at the prompt; Ctrl-C →
  SIGINT to foreground pgid, Ctrl-Z → SIGSTOP. Serial is still
  the primary TTY (VGA+PS/2 path partially wired — see Deferred).
- **Userland**: static + dynamic binaries. Interactive shell with
  history (↑/↓), tab completion against /bin, `$VAR` expansion,
  pipes, `>`/`>>`, `&` backgrounding. `ps`, `lsblk`, `free`,
  `find`, `rm`, `stty`, `cd`, `pwd`, `clear`, `bg`, `help`.
  Real Lua 5.4.7 (static against libvibc). `vi` (static).
- **Dynamic linking**: `ld-lighthos.so.1` at `0x40000000`,
  libs load at first-fit from `0x30000000`, dlopen interface
  published at `DL_IFACE_ADDR=0x50000000`. Handles R_X86_64_
  RELATIVE/64/PC32/PLT32/GLOB_DAT/JUMP_SLOT/COPY with RELA.
- **Test harness**: `make docker-test-disk` — 20 scripts,
  42 assertions, ~6 seconds.

## Working set

```
src/kernel/       elf.c, process.c, syscall.c, syscall_entry.s,
                  task.c, panic.c, isr_stub.s, usermode.s, gdt.c,
                  tss.c, idt.c, pic.c, timer.c, pipe.c, debug.c
src/mm/           pmm.c, vmm.c, heap.c  (heap_block_t is 32-byte
                  padded to keep kmalloc 16-aligned — required
                  for aligned(16) locals on task kernel stacks)
src/fs/           vfs.c + ramfs.c + fat.c + fstab.c + blkdev.c
src/lib/          kprintf.c (boot log), string.c
src/drivers/      serial.c (CSI decoder + echo), vga.c,
                  keyboard.c (PS/2, scancode set 1),
                  console.c (ANSI→VGA mirror; not routed yet),
                  ata.c
user/             all userland sources (.c, .s, .ld, .h)
user/libc/        libvibc — stdio (unbuffered stdout), string,
                  malloc (sbrk-via-mmap_anon), snprintf, setjmp.s,
                  strtod, ctype, locale, errno
user/ldso/        crt0_ldso.s, ld_main.c, ldso.ld
user/libtestdl/   trivial .so used by dlopentest
tests/*.vsh       20 shell-script tests driven by runtests
grub.cfg          normal boot (shell as /bin/init)
grub-test.cfg     test ISO boot — cmdline autorun=/bin/runtests
build/            ALL build artifacts (gitignored)
  build/user/<name>            user binaries
  build/sysroot/usr/lib/       .a / .so artifacts
  build/disk.img               FAT32 disk image
  build/bootdisk.img           MBR + stage2 + kernel + disk
  build/lighthos.iso           normal ISO
  build/lighthos-test.iso      test ISO with autorun
```

## Build + test

Everything runs through Docker (hermetic x86_64-elf toolchain):

```
make docker-build        # kernel + user + ISO
make docker-disk         # build/disk.img (FAT32, populated via mcopy)
make docker-bootdisk     # stamp bootdisk.img
make docker-test-disk    # full test run (~6s; 42 assertions)
make run                 # boot ISO
make run-disk            # boot ISO + disk.img attached
make run-bootdisk        # boot the self-contained disk (shell as init)
make run-installed       # docker-build + disk + bootdisk + run
make run-gdb             # QEMU -s -S, attach x86_64-elf-gdb on :1234
make clean               # rm -rf build + legacy user/*.o stragglers
```

PASS condition: `=== Summary:` line present, no `FAIL`,
`KERNEL FAULT`, or `KERNEL PANIC`. `docker-test-disk` exit code
reflects that.

## Architectural decisions to remember

**4-level paging, higher-half kernel.** Kernel lives at VMA
`0xFFFFFFFF80000000` (LMA at 1 MiB). HHDM window at
`0xFFFF800000000000` covers the low 64 MiB of physical memory, so
any frame the kernel needs to poke (page tables, user stacks via
the current PML4's HHDM alias) is reachable. Heap at
`0xFFFFFFFF81000000` (higher-half so CR3 switches don't hide it).

**Per-process PML4, kernel half shared.** `vmm_new_pml4` copies
entries 256..511 by reference from the boot PML4. User lives in
PML4[0]. Fork is eager: every PDPT/PD/PT gets cloned.

**TSS.rsp0 kept in sync with current task stack.** Also mirrored
into `syscall_kernel_rsp` by `tss_set_kernel_stack` so the SYSCALL
entry stub can switch stacks without needing swapgs.

**SYSCALL entry (`src/kernel/syscall_entry.s`)**. Stashes user rsp
in a module-local scratch word (safe on UP with IF masked via
SFMASK), loads kernel rsp, builds a `registers_t`-shaped frame
exactly like the interrupt path, calls `syscall_handler`, returns
via `iretq`. Exit is iretq not sysretq — keeps symmetry with the
ISR path so yield/schedule work seamlessly.

**GDT ordering matches SYSCALL/SYSRET selector math.**
0x08 kcode, 0x10 kdata, 0x18 user32 placeholder, 0x20 udata,
**0x28 user64**, 0x30 TSS. STAR[63:48]=0x18 implies sysretq CS=0x2B
(0x18+16|3) and SS=0x23 (0x18+8|3). User CS flipped 0x1B→0x2B in
this port.

**Heap 16-byte alignment.** `heap_block_t` is 32 bytes (explicit
pad) so `kmalloc` returns 16-aligned pointers. Without this, task
kernel stacks came back at +24 offset and any `aligned(16)` local
(notably `task_alloc`'s FXSAVE buffer) fault with #GP.

**Dynamic linking layout.** Main exec non-PIE at `0x08048000`
(via `user/user.ld`). ld.so at fixed `0x40000000`. Shared libs
first-fit from `0x30000000`. dlopen interface at `0x50000000`.

## Gotchas (things we learned the hard way)

- **SYSCALL clears IF via IA32_FMASK**, so bare `hlt` in a
  syscall handler would wait for an NMI. Use `sti; hlt; cli` in
  any blocking kernel loop reachable from a syscall (already done
  in `serial_getchar` and `console_read`).
- **`R_X86_64_PLT32`** is emitted for function calls even in
  static-ish TUs. ld.so's relocator must handle it (adds addend
  to `S - P`). Unknown reloc types → loud `[ldso] unknown reloc`
  abort, not silent.
- **SysV AMD64 ABI alignment**: `rsp % 16 == 0` right before the
  user entry point, which means `rsp % 16 == 8` on entry to any
  C function (call pushed 8). The interrupt + syscall paths
  together push 22 qwords before calling `isr_handler` /
  `syscall_handler`, giving the expected 8-mod-16 at call entry.
  Violating it silently corrupts any libgcc path.
- **libvibc vs libulib** both define `puts`/`putchar`/`printf`.
  libulib's are `__attribute__((weak))` so libvibc wins when both
  are linked into a static binary. libulib.so.1 keeps its copies
  so pure-dynamic binaries (DT_NEEDED libulib.so.1) still resolve.
- **Unbuffered stdout**. libvibc's `_stdout` is `_IONBF`. vi
  interleaves raw `u_puts_n` with buffered `printf`; line
  buffering reordered writes and dropped the status bar. If you
  introduce a program that wants batching, call `setvbuf` yourself.
- **Serial echo + shell history.** The kernel's serial line
  discipline echoes printable bytes and tracks `line_len` so
  backspace at the prompt is a no-op. Shell's `repaint` after
  ↑/↓ can desync `line_len` from the visible buffer; known minor
  visual artifact. Full fix needs a raw-mode ioctl.
- **fd offsets are per-process**. Parent and child each have
  their own offset for an inherited fd. Overlapping writes race.

## Deferred (known future work)

- **Merge `x64-port` into `main`.** Not done. Audit in "Audit of
  main vs x64-port" below is clean.
- **`test_badptr` binary**. The i686 tree had a regression test that
  handed the kernel knowingly-bad user pointers and expected `-1`.
  Today's x86_64 syscall dispatcher does not validate user pointers
  at all — it casts `uintptr_t` to a pointer and dereferences — so
  the test would fault the kernel rather than assert. Fixing the
  validation is a prerequisite. Scope: add a user-range check in
  every `SYS_*` case that takes a user pointer.
- **VGA as a genuinely interactive path.** FD_CONSOLE now routes
  through `console_read`/`write` so keyboard + VGA are wired, but
  QEMU is still launched with `-serial stdio` and no VGA-only boot
  entry. Needs a `run-vga` grub target that drops `console=ttyS0`
  from the kernel cmdline. Also want scrollback + attribute support
  in `vga_putchar`.
- **Shell + Lua REPL raw-mode takeover.** `SYS_TTY_RAW` lets user
  code opt in, but neither the shell nor the Lua REPL actually do
  it yet. Picking them up gets us arrow-key history without the
  repaint desync.
- **Init-as-reaper loop.** `process_exit` self-reaps orphans of an
  exiting parent; there's no long-running pid-1 reap loop. The
  current behavior handles `bomb N`; a proper reparent-to-init
  policy would need the init process restructured.
- **Copy-on-write fork**. Fork copies PML4/PDPT/PD/PT eagerly;
  the 6-second test budget has headroom so it's not urgent.
- **LD_LIBRARY_PATH / DT_RPATH**. ld.so only looks in `/lib`.
- **dlclose refcounting, DT_INIT/DT_FINI, lazy binding, TLS,
  symbol versioning** — all deferred.
- **Real blocking `sys_sleep`**. `/bin/sleep` polls `sys_time`.
- **ext2 driver** (would let GRUB boot an ELF directly).
- **uid/gid + permission model**, networking, graphical modes.
- **Richer kernel cmdline** (single-user, log-level).
- **Fortran front-end.** Toolchain is currently `c,c++` only; the
  Dockerfile flip to `c,c++,fortran` is trivial but `libgfortran`
  wants a hosted-C environment we don't provide. A minimal
  `libgfortran_min.a` (stubs for `_gfortran_st_write`,
  `_gfortran_stop_numeric`, etc.) would cover compute-style Fortran
  without namelist / OPEN / derived-type I/O.

## Rescue (static) binary set

These are statically linked so a fresh disk with no `/lib/`
boots to a working shell:

- `shell` — now the default `/bin/init`.
- `hello` — universal smoke test.
- `install` — copies `/bin/*` and `/lib/*.so.1` onto a mounted disk.
- `runtests` — drives in-system tests; invoked by the autorun path.
- `echo` — chroot-test dependency.
- `vi`, `lua`, `ps`, `lsblk`, `free` — all static against
  libvibc (X64_USER_LIBC_TARGETS + X64_USER_EXTRA in Makefile).

Dynamic set: `dynhello`, `dyn_echo`, `dlopentest`. Proof the
ld.so path still works end-to-end (covered by `dynhello.vsh`,
`dyn_echo.vsh`, `ldso_smoke.vsh`, `dlopen.vsh`).

## Audit of `main` vs `x64-port` (done 2026-04-17)

File-level diff audit complete. Findings:

- **Real regressions restored on the port branch**:
  - `user/grep.c` — multi-file support + `<path>:` label prefix were
    lost; now restored. `grep -n foo a b` labels each hit.
  - `user/wc.c` — multi-file support + per-file label + trailing
    `total` line were lost; restored.
  - `src/kernel/process.h` — `SIG_HUP` (number 1) was dropped.
    Re-added alongside the other signal constants. No code delivers
    it yet; it exists purely for ABI continuity so `kill -HUP` and
    any future user handler are wired to the same number as before.
- **Intentional changes, not regressions** (documented here so the
  next audit doesn't re-flag them):
  - `SYS_SBRK` (syscall 45) no longer exists in the kernel. The
    userspace `sys_sbrk` shim in `user/ulib.c` now grows an
    `mmap_anon` arena in 64 KiB chunks. libvibc's malloc keeps
    calling `sys_sbrk` unchanged.
  - `user/shell.c` no longer has a `write <path> <text...>`
    builtin. The shell's `>` / `>>` redirection covers the same
    ground (`echo foo > path`).
  - Pre-port binaries `hexdump`, `pagemap`, `regions`,
    `test_badptr` remain unbuilt. They depend on the three
    unimplemented debug syscalls called out under Deferred.
  - Pre-port kernel files `src/drivers/ramdisk.{c,h}`,
    `src/fs/simplefs.{c,h}`, `src/kernel/gdbstub.{c,h}` were
    deleted. Ramdisk + simplefs are superseded by ATA+FAT32+VFS.
    gdbstub was i386-only; QEMU's `-s -S` replaces it.
  - Pre-port `user/fork_test.c` deleted — it used the old `syscall.h`
    and duplicated functionality in the live `user/test_fork.c`.
  - Test suite grew from **18** `.vsh` scripts on `main` to **20**
    on `x64-port` (`pgroup.vsh`, `xmm.vsh` added). All 18 pre-port
    scripts carried forward verbatim.

## Plan for next session

Audit has been done (see section above). Concrete steps retained
for reference if re-running:

1. `git diff main..x64-port --stat` — top-level scan.
2. `git diff main..x64-port -- user/ src/kernel/syscall.{c,h}
   src/kernel/process.{c,h}` — the regression-prone surface.
3. Cross-check:
   - `#define SYS_*` in `src/kernel/syscall.h` vs `case SYS_*` in
     `src/kernel/syscall.c` — any numbers reserved without
     dispatch are user-visible regressions waiting to bite.
   - Pre-port shell builtins list (see `main`'s
     `user/shell.c:builtins[]`) vs what's live now.
   - Pre-port `X64_USER` list vs current
     `X64_USER + X64_USER_EXTRA + X64_USER_LIBC`.
   - Every `tests/*.vsh` that existed on `main` vs current
     (currently 20 → was 18 pre-port, all carried + 2 new).

Commits worth auditing individually:

- `51176d0` Phase G — removed PORT_MINIMAL + all `*.pre_l5`
  sidecars. Largest deletion pass; highest regression risk.
- `a71bb6d` Port the rest of the pre-port userland — brought back
  `ps`, `lsblk`, `free`, `find`, `bomb`, `install`.
- `3f6bdf9` Final port audit — `cd`/`pwd`/`rm`/`stty`, vi status
  bar, `SYS_CHDIR`/`SYS_GETCWD`.
- `b0e7dd2` Reap orphans + boot_log_flush.

**After the audit, known next tasks:**

- Route FD_CONSOLE through `console_read`/`write` so VGA+PS/2
  becomes a real TTY. Drivers are already initialized.
- Implement a serial raw-mode ioctl (or a toggle read from a
  dedicated fd) so the shell can take over echo and fix the
  history-repaint desync.
- Fill in `SYS_PEEK`/`SYS_PAGEMAP`/`SYS_REGIONS` if we want
  `hexdump`/`pagemap`/`regions` back.
- Consider an init-that-reaps loop so zombies whose parent is
  init (pid 1) get cleaned automatically, not just on their
  parent's exit.
- Merge `x64-port` → `main` once the diff audit is clean.

## First actions for a new agent

1. `git checkout x64-port` (if not there) — `main` is pre-port.
2. `make docker-test-disk` — 6-second sanity check that the tree
   builds and all 42 assertions pass.
3. Read this file + the memory snapshot at
   `~/.claude/projects/-home-stolley-projects-vibeos/memory/`
   (path reflects the repo's current directory).
4. `git log --oneline main..HEAD` — last ~22 commits are the port.
5. Execute the plan above before adding new features.
