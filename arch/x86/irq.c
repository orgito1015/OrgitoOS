/*
 * OrgitoOS - Generic hardware IRQ dispatch (IRQ2-15). See irq.h.
 */

#include "irq.h"
#include "idt.h"
#include "pic.h"
#include "../../kernel/kernel.h"

/* Raw entry stubs (arch/x86/isr.S), one per hardware IRQ line. IRQ0/1
 * have their own long-standing dedicated handlers (kernel/kernel.c);
 * these generic ones cover 2-15 for on-demand drivers like the NIC. */
extern void irq2_handler(void);
extern void irq3_handler(void);
extern void irq4_handler(void);
extern void irq5_handler(void);
extern void irq6_handler(void);
extern void irq7_handler(void);
extern void irq8_handler(void);
extern void irq9_handler(void);
extern void irq10_handler(void);
extern void irq11_handler(void);
extern void irq12_handler(void);
extern void irq13_handler(void);
extern void irq14_handler(void);
extern void irq15_handler(void);

static void (*const irq_stub_table[16])(void) = {
    0, 0,                 /* IRQ0/1: not routed through this table */
    irq2_handler,  irq3_handler,  irq4_handler,  irq5_handler,
    irq6_handler,  irq7_handler,  irq8_handler,  irq9_handler,
    irq10_handler, irq11_handler, irq12_handler, irq13_handler,
    irq14_handler, irq15_handler,
};

static irq_handler_t irq_handlers[16] = {0};

void irq_install_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 2 || irq > 15 || !handler) {
        return;
    }

    irq_handlers[irq] = handler;
    idt_set_gate(0x20 + irq, (uint32_t)irq_stub_table[irq],
                 KERNEL_CODE_SEGMENT, IDT_FLAGS_KERNEL);
    pic_unmask_irq(irq);
}

void irq_uninstall_handler(uint8_t irq) {
    if (irq < 2 || irq > 15) {
        return;
    }
    pic_mask_irq(irq);
    irq_handlers[irq] = 0;
}

void irq_generic_dispatch(int irq) {
    if (irq >= 2 && irq <= 15 && irq_handlers[irq]) {
        irq_handlers[irq]();
    }
    pic_send_eoi((uint8_t)irq);
}
