/*
 * OrgitoOS - Interrupt Service Routines (ISRs)
 */

#ifndef ORGITOOS_ARCH_X86_ISR_H
#define ORGITOOS_ARCH_X86_ISR_H

/* IRQ handlers */
void irq0_handler(void);  /* Timer interrupt */
void irq1_handler(void);  /* Keyboard interrupt */

/* LAPIC timer interrupt (kernel/apic.h LAPIC_TIMER_VECTOR) - per-CPU
 * preemption source for Application Processors, installed by whichever
 * CPU arms its own timer (kernel/smp.c ap_entry()). */
void lapic_timer_handler_stub(void);

/* Generic IRQ2-15 handlers, routed at runtime via arch/x86/irq.c
 * (irq_install_handler) - used by device drivers whose IRQ line isn't
 * known until PCI enumeration, e.g. drivers/rtl8139.c. */
void irq2_handler(void);
void irq3_handler(void);
void irq4_handler(void);
void irq5_handler(void);
void irq6_handler(void);
void irq7_handler(void);
void irq8_handler(void);
void irq9_handler(void);
void irq10_handler(void);
void irq11_handler(void);
void irq12_handler(void);
void irq13_handler(void);
void irq14_handler(void);
void irq15_handler(void);

/* ISR installation */
void isr_install(void);

#endif /* ORGITOOS_ARCH_X86_ISR_H */
