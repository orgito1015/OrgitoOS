/*
 * OrgitoOS - PCI configuration space access (legacy I/O ports
 * 0xCF8/0xCFC mechanism #1). Just enough to find a device by
 * vendor/device ID, read its BARs and interrupt line, and turn on I/O
 * space + bus mastering - what a simple NIC driver (drivers/rtl8139.c)
 * needs.
 */

#ifndef ORGITOOS_KERNEL_PCI_H
#define ORGITOOS_KERNEL_PCI_H

#include <stdint.h>

typedef struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  irq_line;
    uint32_t bar[6];
} pci_device_t;

uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);

/*
 * Scan bus 0 (sufficient for QEMU's flat PCI topology; a real machine
 * with PCI-to-PCI bridges would need a recursive bus walk, out of scope
 * for a single onboard NIC) for the first device matching vendor_id and
 * device_id. Returns 0 and fills *out on success, -1 if not found.
 */
int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out);

/* Set the I/O Space Enable and Bus Master Enable bits in the device's
 * PCI command register (required before a NIC's DMA/PIO will work). */
void pci_enable_bus_mastering(const pci_device_t *dev);

#endif /* ORGITOOS_KERNEL_PCI_H */
