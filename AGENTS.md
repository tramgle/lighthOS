# LighthOS — agent launch point

Quick orientation doc for future sessions. Read this first; pair
with the long-form memory snapshot in Claude Code's auto-memory
(path derived from the repo's absolute location — if still at
`~/projects/vibeos`, it's
`.claude/projects/-home-stolley-projects-vibeos/memory/project_vibeos.md`)
for the long-form history.

## What this is

Hobby i686 kernel written in C + a little nasm. User programs are
separate ELFs loaded by the kernel. As of HEAD we have:

- Kernel: paging, heap, tasks + per-process page directories, VFS,
  FAT32 r/w + LFN, ATA, pipes, signals (F4: user handlers, alarm),
  strace, mount/umount, boot-log-to-FAT, panic-log-to-FAT, serial
  gdb stub on COM2.
- User: ~30 dynamic + 4 static binaries, a shell with job control,
  lua 5.4 as a shared-lib binary, vi clone.
- Dynamic linking: kernel PT_INTERP handling, user-space
  `ld-lighthos.so.1`, `libulib.so.1`, `libvibc.so.1`, `libtestdl.so.1`.
  dlopen/dlsym via a fixed-address function table at 0x50000000.
- Environment variables end-to-end via SysV auxv + ulib getenv/setenv.
- Tier-3 test harness: kernel reads `autorun=<path>` from multiboot
  cmdline, spawns runtests, ACPI shutdown on exit. No stdin hacks.
  18 tests, 40 assertions, ~6-second run.

## Working set

```
src/kernel/       core — elf.c, process.c, syscall.c, task.c, gdbstub.c, panic.c
src/mm/           pmm, vmm (4-entry kernel PDEs shared, user PDE 4-1023 private)
src/fs/           vfs + ramfs + fat.c + fstab.c (declarative mount)
src/lib/          kprintf with boot-log capture
src/drivers/      serial, vga, ata, console, keyboard
user/             sources (*.c, *.h, *.s, *.ld) only — no build artifacts
user/libc/        subset libc source (malloc, stdio, strtod, snprintf, setjmp...)
user/ldso/        the dynamic linker: crt0_ldso.s, ld_main.c, ldso.ld
user/libtestdl/   trivial .so used by dlopentest
tests/*.vsh       shell-script tests driven by runtests
etc/fstab         ISO-time fstab (built-in default for bootdisk)
grub.cfg          normal boot
grub-test.cfg     test ISO boot — cmdline has autorun=/tests
build/            ALL build artifacts (gitignored)
  build/user/<name>           user binaries
  build/user/*.o              user object files
  build/user/libc/*.{o,pic.o} libc objects
  build/user/ldso/*.o         ld.so objects
  build/sysroot/usr/include/  staged headers
  build/sysroot/usr/lib/      .a / .so artifacts
  build/disk.img              FAT32 disk image
  build/bootdisk.img          self-bootable disk (disk.img + MBR + kernel)
  build/lighthos.iso            normal ISO
  build/lighthos-test.iso       test ISO with autorun=/tests
```

## Build + test

Everything runs through Docker so the toolchain is hermetic:

```
make docker-build        # kernel + user + ISO
make docker-disk         # build/disk.img (FAT32, populated via mcopy)
make docker-bootdisk     # stamp bootdisk.img
make docker-test-disk    # full test run (~6s, tier-3 harness)
make run                 # boot ISO
make run-disk            # boot ISO + disk.img attached
make run-bootdisk        # boot the self-contained disk
make run-installed       # docker-build + docker-disk + docker-bootdisk + run
make run-gdb             # boot ISO with gdb stub on COM2 = tcp::1234
make clean               # rm -rf build + legacy user/*.o stragglers
```

The test harness PASSES if it sees a valid `=== Summary:` line AND
no `FAIL`, `KERNEL PANIC`, or missing-summary conditions.
`make docker-test-disk` exit code reflects that.

## Architectural decisions to remember

**Per-process page directories.** Kernel PDEs 0..3 shared across every
process (identity-map of first 16 MB). PDE 4+ is private. The
"first 16 MB for everything kernel-touchable" rule is enforced by
`check_low()` in `src/mm/vmm.c`; frames + PDs + PTs live there.

**Install mounts at `/` directly.** Not at `/disk`. Project R
untangled a confused original design. `fstab_mount_defaults()` in
`src/fs/fstab.c` emits `ata0p0 / fat rw`; ISO boots use the ramfs
`/etc/fstab` which points disk at `/disk` for dev convenience.

**SysV System V i386 ABI for user-program entry.** Stack at entry:
argc, argv (contiguous), NULL, envp (contiguous), NULL, auxv pairs,
AT_NULL 0. Auxv carries AT_PHDR/PHENT/PHNUM/BASE/ENTRY/PAGESZ.
crt0.s extracts all three for `main(int argc, char **argv, char
**envp)`. "SysV" here ≠ SysV init system.

**Non-PIE main executable.** Fixed at 0x08048000 via `user/user.ld`.
ld.so handles ET_DYN shared libs (PIC, loaded at 0x30000000+).
Interpreter is a static ET_EXEC pinned at 0x40000000. See
"Gotchas" below re R_386_COPY.

**Kernel is NOT PIC and runs at virtual ~0x100000 in the identity
map.** Switching that requires the L5 port.

## Gotchas (things we learned the hard way)

- **i686-elf gcc swallows bare `-shared`.** Must use `-Wl,-shared`
  to force pass-through. Same for `-Bstatic` / `-Bdynamic` toggles.
- **`-ffreestanding -nostdlib` links the default linker script,
  which for our target adds PT_INTERP and .dynamic sections even
  when unwanted.** The ldso build uses `-static --no-dynamic-linker
  -Bstatic --build-id=none` to produce a clean ET_EXEC.
- **R_386_COPY + symbol interposition.** For each data symbol main
  imports from a library (e.g. `environ`, `stdin`, `errno`), the
  linker reserves a slot in main's `.dynbss` and emits R_386_COPY.
  ld.so must copy the library's initial bytes into main's slot AND
  make library accesses go through main's slot. `resolve_symbol` in
  `user/ldso/ld_main.c` checks g_main first. `R_386_COPY` itself
  must SKIP g_main when looking up its source (otherwise it's a
  no-op copy to itself). See the big comment in apply_rel.
- **GNU ld synthesizes NOTYPE pseudo-symbols in main's .dynsym for
  imported-function address-taking.** They land in .got.plt.
  `lookup_in` must reject `STT_NOTYPE` to avoid resolving library
  PLT calls to data-section addresses. Nasm-defined functions
  (setjmp/longjmp in `user/libc/setjmp.s`) must be declared
  `global foo:function` so the assembler emits `STT_FUNC`.
- **fd offsets are per-process.** Parent and child each have their
  own offset counter for the same inherited fd. Simultaneous writes
  to the same file will overwrite each other. Test scripts avoid
  this by either separate files or sequencing writes pre-spawn.
- **Test harness uses `autorun=/tests` multiboot cmdline** parsed
  by `src/kernel/main.c`. No more stdin timing hacks.
- **Every grub.cfg entry is a multiboot module.** Paths with `/` are
  preserved verbatim into the ramfs. Parent dir auto-mkdir'd (one
  level). See `src/kernel/main.c:121-180`.
- **Build artifacts must NOT land in `user/`.** `.gitignore` has
  `user/**/*.o` etc. as a safety net; the Makefile routes all
  outputs into `build/user/` via `$(BUILD_USER)`. If you write a
  new rule, use that prefix.

## Dynamic-linking status quick ref

- Non-PIE main exec at 0x08048000. ld-lighthos.so.1 at 0x40000000.
  Shared libs at first-fit from 0x30000000.
- Relocation types handled: R_386_RELATIVE, R_386_GLOB_DAT,
  R_386_JMP_SLOT, R_386_32, R_386_PC32, R_386_COPY.
- DT_NEEDED is walked recursively; cycle-safe via basename dedupe.
- Main-first symbol interposition (R_386_COPY correctness).
- dlopen/dlsym/dlclose/dlerror via function table at
  `DL_IFACE_ADDR=0x50000000`. dlclose is a no-op stub (no refcounts).
- DT_RPATH, LD_LIBRARY_PATH, lazy binding, TLS, DT_INIT/DT_FINI,
  symbol versioning: **deferred**.

## Deferred (known future work)

- **L5: 64-bit port.** Genuinely multi-session. New toolchain,
  long-mode bootstrap, pointer-size audit, ELF64 loader, ld.so
  rewrite, user-space rebuild, multiboot2 or 32→64 trampoline.
- LD_LIBRARY_PATH / DT_RPATH (easy once envp is through — envp is
  through, just haven't wired it to the linker search).
- dlclose refcounting + unload.
- DT_INIT / DT_FINI (library ctors/dtors).
- Lazy binding / PLT trampoline.
- ext2 driver (would let GRUB boot the kernel ELF directly).
- Permission model (uid/gid + superuser).
- Networking, graphical modes.
- Kernel cmdline richer than just `autorun=` (e.g., single-user,
  log-level).
- Real blocking `sys_sleep` (today `/bin/sleep` polls sys_time).

## Rescue (static) binary set

These stay statically linked so a fresh disk with no `/lib/` boots
to a working shell:

- `hello` — universal smoke test.
- `install` — copies `/bin/*` and `/lib/*.so.1` onto a mounted disk.
- `runtests` — drives in-system tests; invoked by the autorun path.
- `echo` — chroot-test dependency (a chroot jail without
  `/lib/` staged inside can't run dynamic binaries).

Anything else is in the dynamic set. Lua, libc_test, vi, shell,
etc. all go through ld-lighthos.so.1 today.

## First actions for a new agent

1. `make docker-test-disk` — 6-second sanity check that the tree
   builds and all 40 assertions pass.
2. Read this file + the memory snapshot at
   `~/.claude/projects/-home-stolley-projects-vibeos/memory/project_vibeos.md`
   (path reflects the repo's current directory, not the project name).
3. Check `git log --oneline` for recent work.
4. If starting a new feature, follow the existing pattern:
   kernel support + user wrapper + test binary + `tests/*.vsh` +
   grub-test.cfg entries + Makefile wiring.
