/*
 * OrgitoOS - Global Descriptor Table (GDT) and Task State Segment (TSS)
 *
 * Phase 1 installs the kernel's own GDT instead of relying on the
 * bootloader's. The layout provides ring 0 and ring 3 segments plus
 * a TSS, which is required for ring 3 -> ring 0 transitions (the CPU
 * loads SS0:ESP0 from the TSS when an interrupt or syscall arrives
 * while executing user code).
 *
 * Per-CPU scheduling (process/percpu.h) needs a *separate* TSS per CPU -
 * the CPU sets a loaded TSS descriptor's busy bit, so a second `ltr` of
 * the same selector on another CPU while the first is still "busy" would
 * fault. MAX_CPUS descriptor slots are reserved unconditionally (SMP
 * topology isn't known yet when gdt_init() runs, well before smp_init());
 * only CPU 0's (index 0, selector GDT_TSS_BASE - unchanged from the
 * single-TSS layout this replaces) is loaded on the BSP by gdt_init()
 * itself. Each Application Processor loads its own selector via
 * tss_flush_for_cpu() as it comes online (kernel/smp.c ap_entry()).
 *
 * Selector layout:
 *   0x00  null
 *   0x08  kernel code   (ring 0)
 *   0x10  kernel data   (ring 0)
 *   0x18  user code     (ring 3, use selector 0x1B = 0x18 | RPL 3)
 *   0x20  user data     (ring 3, use selector 0x23 = 0x20 | RPL 3)
 *   0x28  TSS for CPU 0 (BSP); 0x28 + 8*cpu_id for every other CPU
 */

#ifndef ORGITOOS_ARCH_X86_GDT_H
#define ORGITOOS_ARCH_X86_GDT_H

#include <stdint.h>
#include "../../include/smp.h"

/* Segment selectors */
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_CODE    0x18
#define GDT_USER_DATA    0x20
#define GDT_TSS_BASE     0x28
#define GDT_TSS          GDT_TSS_BASE   /* legacy alias: CPU 0's TSS selector */

/* TSS selector for a given CPU index (0 == GDT_TSS_BASE, matching the
 * original single-CPU layout exactly). */
#define GDT_TSS_SELECTOR(cpu_id) (uint16_t)(GDT_TSS_BASE + (cpu_id) * 8)

/* Ring 3 selectors (RPL = 3) as loaded into segment registers */
#define USER_CODE_SELECTOR (GDT_USER_CODE | 3)  /* 0x1B */
#define USER_DATA_SELECTOR (GDT_USER_DATA | 3)  /* 0x23 */

/* GDT entry (segment descriptor) */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

/* GDT pointer for lgdt */
struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* 32-bit Task State Segment */
struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;      /* Kernel stack pointer loaded on ring 3 -> 0 */
    uint32_t ss0;       /* Kernel stack segment loaded on ring 3 -> 0 */
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

/* Install the kernel GDT and TSS. Call before enabling interrupts. */
void gdt_init(void);

/* Load a GDT pointer (arch/x86/context.S): lgdt + reload every segment
 * register, including CS via a far jump. Exposed here (not just as an
 * internal `extern` in gdt.c) so kernel/smp.c's ap_entry() can load the
 * same already-built kernel GDT on each Application Processor. */
void gdt_flush(uint32_t gdtp_addr);

/* Update *this CPU's own* TSS kernel stack pointer (call on every context
 * switch, on whichever CPU is doing the switching). */
void tss_set_kernel_stack(uint32_t esp0);

/* Load TSS selector GDT_TSS_SELECTOR(cpu_id) into this CPU's task
 * register. Every Application Processor must call this for its own
 * cpu_id exactly once, as it comes online (kernel/smp.c ap_entry()) -
 * the BSP's equivalent (cpu_id 0) already happens inside gdt_init() via
 * the existing tss_flush(). */
void tss_flush_for_cpu(uint32_t cpu_id);

/* Address of the already-built kernel gdt_ptr (kernel/apic.c's AP entry
 * path reuses it via gdt_flush(), rather than building a second,
 * independent GDT for Application Processors). */
uint32_t gdt_get_ptr_addr(void);

#endif /* ORGITOOS_ARCH_X86_GDT_H */
