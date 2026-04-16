/*
 * L4 kernel_main — loads an ELF64 user binary, drops to ring 3,
 * handles its INT 0x80 syscalls.
 *
 * GRUB passes hello_l4 as a multiboot module. We allocate a fresh
 * PML4 (sharing kernel half), elf_load into it, set CR3, build a
 * user stack, jump_to_usermode. The L4 user binary does two fake
 * syscalls — a "write this buffer" and an "exit" — distinguished
 * by sentinel RAX values. No real syscall dispatcher yet; that's
 * L5 + userland ABI work.
 */

#include "include/types.h"
#include "include/multiboot.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "kernel/gdt.h"
#include "kernel/tss.h"
#include "kernel/idt.h"
#include "kernel/isr.h"
#include "kernel/pic.h"
#include "kernel/elf.h"

struct task;
struct task *task_current(void) { return (void *)0; }
uint64_t    *g_user_pml4;
uint64_t    *task_current_pml4(void) { return g_user_pml4; }

extern void serial_init(void);
extern void jump_to_usermode(uint64_t entry, uint64_t user_stack);
extern char stack_top;

#define USER_STACK_TOP 0x0000000000800000ULL  /* 8 MiB VA, one page below */
#define USER_STACK_PAGES 4

/* L4 sentinel opcodes — see user/hello_l4.s. */
#define L4_OP_WRITE  0x4C54
#define L4_OP_EXIT   0x4C58

static volatile int l4_user_done;
static volatile int l4_exit_code;

static registers_t *syscall_l4(registers_t *regs) {
    switch (regs->rax) {
    case L4_OP_WRITE: {
        const char *s = (const char *)(uintptr_t)regs->rdi;
        uint64_t n = regs->rsi;
        serial_printf("[l4] user wrote: ");
        for (uint64_t i = 0; i < n; i++) {
            char c = s[i];
            if (c == '\n') serial_printf("\\n");
            serial_printf("%c", c);
        }
        serial_printf("\n");
        regs->rax = (uint64_t)n;
        return regs;
    }
    case L4_OP_EXIT:
        l4_user_done = 1;
        l4_exit_code = (int)regs->rdi;
        serial_printf("[l4] user exit: code=%d\n", l4_exit_code);
        for (;;) __asm__ volatile ("cli; hlt");
    default:
        serial_printf("[l4] unknown syscall rax=0x%lx\n", regs->rax);
        return regs;
    }
}

static void *multiboot_first_module(multiboot_info_t *mbi, uint32_t *size_out) {
    if (!(mbi->flags & MULTIBOOT_FLAG_MODS) || mbi->mods_count == 0) return 0;
    multiboot_mod_t *m = (multiboot_mod_t *)(uintptr_t)mbi->mods_addr;
    *size_out = m->mod_end - m->mod_start;
    return (void *)(uintptr_t)m->mod_start;
}

static void l4_run_user(multiboot_info_t *mbi) {
    uint32_t mod_size = 0;
    void *mod = multiboot_first_module(mbi, &mod_size);
    if (!mod) { serial_printf("[l4] no module loaded\n"); return; }
    serial_printf("[l4] module @0x%lx size=%u\n",
                  (uint64_t)(uintptr_t)mod, mod_size);

    if (elf_validate(mod, mod_size) != 0) {
        serial_printf("[l4] module is not a valid ELF64 x86-64 binary\n");
        return;
    }

    uint64_t *pml4 = vmm_new_pml4();
    g_user_pml4 = pml4;
    uint64_t entry = elf_load(mod, mod_size, pml4);
    if (!entry) { serial_printf("[l4] elf_load failed\n"); return; }
    serial_printf("[l4] entry=0x%lx\n", entry);

    /* Build a user stack: 4 pages at USER_STACK_TOP - USER_STACK_PAGES*4K. */
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t va = USER_STACK_TOP - (i + 1) * PAGE_SIZE;
        uint64_t frame = pmm_alloc_frame();
        memset(phys_to_virt_low(frame), 0, PAGE_SIZE);
        vmm_map_in(pml4, va, frame, VMM_FLAG_WRITE | VMM_FLAG_USER);
    }

    /* Switch to user PML4 and install INT 0x80 handler. */
    vmm_switch_pml4(pml4);
    isr_register_handler(0x80, syscall_l4);

    /* Set tss.rsp0 to our kernel stack so the CPU switches there
       on the user→kernel transition. */
    tss_set_kernel_stack((uint64_t)(uintptr_t)&stack_top);

    serial_printf("[l4] dropping to ring 3...\n");
    jump_to_usermode(entry, USER_STACK_TOP - 16);
}

void kernel_main(uint32_t magic, multiboot_info_t *mbi) {
    serial_init();

    serial_printf("\n================================\n");
    serial_printf("LighthOS L4: ELF64 + ring 3\n");
    serial_printf("================================\n");

    if (magic != MULTIBOOT_MAGIC) {
        serial_printf("bad multiboot magic; halting.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    pmm_init(mbi);
    vmm_init();
    gdt_init();
    tss_init((uint64_t)(uintptr_t)&stack_top);
    pic_init();
    idt_init();

    l4_run_user(mbi);

    serial_printf("\n[l4] fell through. halting.\n");
    for (;;) __asm__ volatile ("cli; hlt");
}
