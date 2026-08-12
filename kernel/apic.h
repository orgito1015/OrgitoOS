/*
 * OrgitoOS - ACPI table parsing (just enough for MADT) + Local APIC
 * access, used by kernel/smp.c to bring up Application Processors.
 */

#ifndef ORGITOOS_KERNEL_APIC_H
#define ORGITOOS_KERNEL_APIC_H

#include <stdint.h>

#define APIC_MAX_ENTRIES 16   /* matches include/smp.h's MAX_CPUS */

/*
 * Locate the ACPI RSDP -> RSDT -> MADT and collect every enabled
 * Processor Local APIC entry's APIC ID into `out_apic_ids` (capacity
 * `max`). Also fills `*out_lapic_phys` with the Local APIC's MMIO base
 * address from the MADT. Returns the number of APIC IDs found, or 0 if
 * ACPI tables could not be located/parsed (e.g. a non-ACPI BIOS) - the
 * caller falls back to single-CPU (BSP only) operation in that case.
 */
uint32_t acpi_collect_local_apic_ids(uint32_t *out_apic_ids, uint32_t max,
                                      uint32_t *out_lapic_phys);

/* Map the Local APIC's MMIO page (uncached, supervisor-only) into the
 * kernel's address space at its own physical address (identity mapped,
 * consistent with the rest of the kernel) and enable it globally via
 * IA32_APIC_BASE. Call once, from the BSP, before touching any lapic_*
 * function below. */
void lapic_init(uint32_t lapic_phys);

/* This CPU's own Local APIC ID (reads the LAPIC ID register - correct
 * for whichever CPU calls it, BSP or AP). */
uint32_t lapic_id(void);

/* Software-enable this CPU's Local APIC (Spurious Interrupt Vector
 * Register) - required on every CPU, not just the BSP. */
void lapic_enable(void);

/* Send the INIT-SIPI-SIPI startup sequence to wake the AP with the given
 * APIC ID, vectoring it to real-mode CS:IP = (vector << 8):0000, i.e.
 * physical address `vector << 12`. Blocks for the standard (~10 ms/200 us)
 * inter-IPI delays; returns once both SIPIs have been accepted by the
 * local APIC (delivery status bit clear), not once the AP has actually
 * booted - the caller polls for that separately. */
void lapic_send_init_sipi_sipi(uint32_t dest_apic_id, uint8_t vector);

/* ------------------------------------------------------------------ */
/* Local APIC timer - per-CPU preemption source for Application         */
/* Processors (process/scheduler.h: the BSP keeps using the PIT).       */
/* ------------------------------------------------------------------ */

/* IDT vector the LAPIC timer's interrupt is installed at (every CPU,
 * arch/x86/isr.S lapic_timer_handler_stub / kernel/kernel.c). Distinct
 * from 0x20 (PIT/IRQ0) and 0x21 (keyboard/IRQ1) - this interrupt is
 * local to whichever CPU it fires on, never routed through the legacy
 * PIC, so there's no IRQ-number/PIC-mask concept for it at all. */
#define LAPIC_TIMER_VECTOR 0x40

/*
 * Calibrate the LAPIC timer against the PIT (already ticking at 100 Hz -
 * drivers/timer.c): run the LAPIC timer at its fastest (divide-by-1) for
 * exactly one PIT tick (~10 ms) and measure how far it counted down from
 * its maximum, giving a "ticks per 10 ms" figure. Call once, on the BSP,
 * after both timer_init() and lapic_init() have run. The result is
 * reused on every CPU (lapic_timer_start()) - real hardware and QEMU
 * both run every core's LAPIC off the same bus clock, so one CPU's
 * calibration is valid for all of them.
 */
void lapic_timer_calibrate(void);

/* Program *this* CPU's own LAPIC timer for periodic interrupts at
 * LAPIC_TIMER_VECTOR, using the count lapic_timer_calibrate() measured.
 * Call once per CPU (BSP and every AP) after lapic_timer_calibrate() has
 * run at least once. No-op (does not arm the timer) if calibration never
 * ran - matches this codebase's "degrade gracefully, don't hang boot"
 * convention for optional hardware features. */
void lapic_timer_start(void);

/* Signal end-of-interrupt to *this* CPU's own Local APIC - call at the
 * top of the LAPIC timer interrupt handler, same role pic_send_eoi()
 * plays for legacy PIC-routed IRQs. */
void lapic_send_eoi(void);

#endif /* ORGITOOS_KERNEL_APIC_H */
