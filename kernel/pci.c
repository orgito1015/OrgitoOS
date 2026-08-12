/*
 * OrgitoOS - PCI configuration space access. See pci.h.
 */

#include "pci.h"
#include "../arch/x86/ports.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static void pci_set_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (1u << 31)
                      | ((uint32_t)bus  << 16)
                      | ((uint32_t)slot << 11)
                      | ((uint32_t)func << 8)
                      | (offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    pci_set_address(bus, slot, func, offset);
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    pci_set_address(bus, slot, func, offset);
    return inw(PCI_CONFIG_DATA + (offset & 2));
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    pci_set_address(bus, slot, func, offset);
    return inb(PCI_CONFIG_DATA + (offset & 3));
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    pci_set_address(bus, slot, func, offset);
    outw(PCI_CONFIG_DATA + (offset & 2), value);
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out) {
    for (uint16_t slot = 0; slot < 32; slot++) {
        uint8_t max_func = 1;

        for (uint8_t func = 0; func < max_func; func++) {
            uint16_t vid = pci_config_read16((uint8_t)0, (uint8_t)slot, func, 0x00);
            if (vid == 0xFFFF) {
                continue;   /* No device in this slot/function */
            }

            if (func == 0) {
                uint8_t header_type = pci_config_read8(0, (uint8_t)slot, 0, 0x0E);
                if (header_type & 0x80) {
                    max_func = 8;   /* Multi-function device: probe 1-7 too */
                }
            }

            uint16_t did = pci_config_read16(0, (uint8_t)slot, func, 0x02);
            if (vid != vendor_id || did != device_id) {
                continue;
            }

            out->bus = 0;
            out->slot = (uint8_t)slot;
            out->func = func;
            out->vendor_id = vid;
            out->device_id = did;

            uint32_t class_reg = pci_config_read32(0, (uint8_t)slot, func, 0x08);
            out->revision   = (uint8_t)(class_reg & 0xFF);
            out->prog_if    = (uint8_t)((class_reg >> 8) & 0xFF);
            out->subclass   = (uint8_t)((class_reg >> 16) & 0xFF);
            out->class_code = (uint8_t)((class_reg >> 24) & 0xFF);

            for (int b = 0; b < 6; b++) {
                out->bar[b] = pci_config_read32(0, (uint8_t)slot, func, (uint8_t)(0x10 + b * 4));
            }
            out->irq_line = pci_config_read8(0, (uint8_t)slot, func, 0x3C);

            return 0;
        }
    }

    return -1;
}

void pci_enable_bus_mastering(const pci_device_t *dev) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1u << 0);   /* I/O Space Enable */
    cmd |= (1u << 2);   /* Bus Master Enable */
    pci_config_write16(dev->bus, dev->slot, dev->func, 0x04, cmd);
}
