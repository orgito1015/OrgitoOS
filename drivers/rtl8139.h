/*
 * OrgitoOS - Realtek RTL8139 NIC driver.
 *
 * QEMU's classic default/simplest emulated NIC and the subject of most
 * OSDev driver-writing tutorials: a small set of I/O-port registers
 * (no MMIO BAR needed) driving one RX ring buffer and four TX
 * descriptor slots. Real packet transmit/receive over real PCI
 * hardware (emulated, but programmed exactly like the real chip) -
 * see kernel/network.c for the Ethernet/ARP/IP/ICMP/UDP layers built
 * on top of it.
 */

#ifndef ORGITOOS_DRIVERS_RTL8139_H
#define ORGITOOS_DRIVERS_RTL8139_H

#include <stdint.h>
#include <stddef.h>

/* Called once with each received Ethernet frame (destination MAC
 * through to the end of the payload, CRC already stripped/ignored) -
 * kernel/network.c registers net_handle_frame() here. Runs in IRQ
 * context (see drivers/rtl8139.c's IRQ handler), so callbacks must stay
 * quick and non-blocking. */
typedef void (*rtl8139_rx_callback_t)(const uint8_t *frame, uint16_t len);

/* Probe PCI for an RTL8139, reset and program it (RX ring, TX
 * descriptors, IRQ line), and enable RX+TX. Returns 0 on success, -1 if
 * no RTL8139 is present (net_init() falls back to "no NIC" in that
 * case - see kernel/network.c). */
int rtl8139_init(rtl8139_rx_callback_t rx_callback);

/* This NIC's burned-in MAC address (valid only after a successful
 * rtl8139_init()). */
void rtl8139_get_mac(uint8_t out_mac[6]);

/* Transmit one Ethernet frame (header through payload; the driver adds
 * no framing of its own). Returns the length sent, or -1 if no TX
 * descriptor is currently free or the frame is oversized. */
int rtl8139_send(const void *data, uint16_t len);

#endif /* ORGITOOS_DRIVERS_RTL8139_H */
