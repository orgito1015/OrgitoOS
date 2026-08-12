/*
 * OrgitoOS - Generic hardware IRQ dispatch (IRQ2-15).
 *
 * IRQ0 (timer) and IRQ1 (keyboard) already have their own dedicated
 * handlers wired directly in kernel/kernel.c (arch/x86/isr.S's
 * irq0_handler/irq1_handler). Real device drivers - the RTL8139 NIC
 * being the first one - land on whatever legacy IRQ line the BIOS/PCI
 * bus assigned them at boot (read from PCI config space, not known
 * until the device is found), so a fixed-at-compile-time handler like
 * IRQ0/1's doesn't fit. This is the same "one handler per line,
 * install and unmask on demand" mechanism generalized for that case.
 */

#ifndef ORGITOOS_ARCH_X86_IRQ_H
#define ORGITOOS_ARCH_X86_IRQ_H

#include <stdint.h>

typedef void (*irq_handler_t)(void);

/* Route hardware IRQ `irq` (0-15) to `handler`: installs the matching
 * IDT gate, remembers the C callback, and unmasks the line on the PIC.
 * Called by a device driver once it knows its IRQ line (e.g. from PCI
 * config space's Interrupt Line register). */
void irq_install_handler(uint8_t irq, irq_handler_t handler);

/* Stop routing `irq` to any handler and mask it again. */
void irq_uninstall_handler(uint8_t irq);

/* Called from arch/x86/isr.S's generic irqN_handler stubs; dispatches
 * to whatever was registered via irq_install_handler() and sends EOI.
 * Not meant to be called directly by driver code. */
void irq_generic_dispatch(int irq);

#endif /* ORGITOOS_ARCH_X86_IRQ_H */
