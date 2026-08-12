/*
 * OrgitoOS - Application Processor real-mode trampoline.
 *
 * An AP, on receiving a STARTUP IPI (SIPI) with vector V, begins
 * executing in real mode at CS:IP = (V << 8):0000, i.e. physical address
 * V << 12 - always below 1 MiB, always 4 KiB-aligned. This file is that
 * code: it gets the AP from 16-bit real mode into 32-bit protected mode
 * with paging on, using the exact same page directory the BSP is
 * already running with, then jumps to the C entry point ap_entry()
 * (kernel/smp.c).
 *
 * Built as its own tiny freestanding executable, linked at a *fixed*
 * physical address (arch/x86/trampoline.ld sets `. = 0x8000`) completely
 * independent of where the kernel image itself loads. That is not
 * optional here: this code runs before it has any GDT of its own or
 * paging, so every absolute address it references (the temporary GDT
 * pointer, the far jump target) must already be correct at whatever
 * physical address the bytes end up sitting at. kernel/smp.c copies the
 * assembled bytes verbatim to physical 0x8000 at boot (which is why the
 * link address and the runtime copy address must match), the same way
 * kernel/exec.c embeds a userland ELF binary rather than compiling it
 * into the kernel image proper.
 *
 * AP_MAILBOX_{PAGEDIR,STACK,ENTRY} below are fixed, hard-coded physical
 * addresses (NOT symbols resolved by the linker) that kernel/smp.c
 * pokes with this boot's page directory, stack, and C entry point
 * immediately before sending an AP's SIPI. They live just below this
 * trampoline's own 0x8000 load address, deliberately outside it: the
 * kernel embeds this file as a symbol-free flat blob (`objcopy -O
 * binary`, same technique as kernel/exec.c's userland binary), so any
 * label defined in here is invisible to C - hard-coded shared constants
 * sidestep that instead of trying to recover stripped symbol offsets.
 * MUST match kernel/smp.c's AP_MAILBOX_* #defines exactly.
 */

.set AP_MAILBOX_PAGEDIR, 0x7FF0
.set AP_MAILBOX_STACK,   0x7FF4
.set AP_MAILBOX_ENTRY,   0x7FF8

.code16gcc

.section .text

.global ap_trampoline_start
.global ap_trampoline_end

ap_trampoline_start:
    cli
    cld

    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    lgdt ap_gdt_ptr

    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0

    ljmp $0x08, $ap_pm32

.align 8
ap_gdt:
    /* 0x00: null */
    .quad 0
    /* 0x08: code - base 0, limit 4 GiB, ring 0, 32-bit, exec/read */
    .word 0xFFFF, 0x0000
    .byte 0x00, 0x9A, 0xCF, 0x00
    /* 0x10: data - base 0, limit 4 GiB, ring 0, 32-bit, read/write */
    .word 0xFFFF, 0x0000
    .byte 0x00, 0x92, 0xCF, 0x00
ap_gdt_end:

ap_gdt_ptr:
    .word ap_gdt_end - ap_gdt - 1
    .long ap_gdt

.code32
ap_pm32:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Same page directory the BSP already runs with - identity-maps low
     * memory (including this very page) and the kernel image, so the
     * C entry point below (a normal >1 MiB kernel address) is reachable
     * the instant paging turns on. */
    mov AP_MAILBOX_PAGEDIR, %eax
    mov %eax, %cr3

    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0

    mov AP_MAILBOX_STACK, %esp
    mov $0, %ebp

    mov AP_MAILBOX_ENTRY, %eax
    jmp *%eax
    /* Not reached: ap_entry() (kernel/smp.c) is noreturn. */

ap_trampoline_end:
