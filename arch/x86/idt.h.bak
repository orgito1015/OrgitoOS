/*
 * OrgitoOS - Interrupt Descriptor Table (IDT)
 */

#ifndef ORGITOOS_ARCH_X86_IDT_H
#define ORGITOOS_ARCH_X86_IDT_H

#include <stdint.h>

/* IDT Gate Descriptor */
struct idt_entry {
    uint16_t offset_low;   /* Lower 16 bits of handler function address */
    uint16_t selector;     /* Kernel segment selector */
    uint8_t  zero;         /* Always 0 */
    uint8_t  type_attr;    /* Type and attributes */
    uint16_t offset_high;  /* Upper 16 bits of handler function address */
} __attribute__((packed));

/* IDT Pointer */
struct idt_ptr {
    uint16_t limit;        /* Size of IDT - 1 */
    uint32_t base;         /* Address of IDT */
} __attribute__((packed));

/* Initialize the IDT */
void idt_init(void);

/* Set an IDT gate */
void idt_set_gate(uint8_t num, uint32_t handler, uint16_t selector, uint8_t flags);

/* Load the already-built kernel IDT into *this* CPU's IDTR. Every CPU
 * needs its own `lidt`, even though they all point at the same shared
 * table in memory - kernel/apic.c's AP entry path calls this to give
 * each Application Processor a working IDT without building a second,
 * independent one. */
void idt_reload(void);

#endif /* ORGITOOS_ARCH_X86_IDT_H */
