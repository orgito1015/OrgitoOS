/*
 * OrgitoOS - Realtek RTL8139 NIC driver implementation. See rtl8139.h.
 *
 * Register offsets and the RX-ring/TX-descriptor programming sequence
 * below follow the chip's well-documented I/O-port interface (the same
 * one every "write an RTL8139 driver" OSDev tutorial programs against).
 */

#include "rtl8139.h"
#include "console.h"
#include "../arch/x86/ports.h"
#include "../arch/x86/irq.h"
#include "../kernel/pci.h"
#include "../kernel/string.h"
#include "../memory/heap.h"

/* Register offsets from the I/O base (BAR0) */
#define REG_MAC0     0x00   /* IDR0-5: 6-byte burned-in MAC address     */
#define REG_RBSTART  0x30   /* RX ring buffer physical start address   */
#define REG_CMD      0x37   /* Command register                        */
#define REG_CAPR     0x38   /* Current Address of Packet Read           */
#define REG_IMR      0x3C   /* Interrupt Mask Register                  */
#define REG_ISR      0x3E   /* Interrupt Status Register                */
#define REG_TCR      0x40   /* Transmit Config Register                 */
#define REG_RCR      0x44   /* Receive Config Register                  */
#define REG_CONFIG1  0x52
#define REG_TSAD0    0x20   /* Transmit Start Address, descriptors 0-3  */
#define REG_TSD0     0x10   /* Transmit Status, descriptors 0-3         */

#define CMD_RST  0x10
#define CMD_RE   0x08
#define CMD_TE   0x04
#define CMD_BUFE 0x01

#define ISR_ROK  0x0001
#define ISR_TOK  0x0004

#define TSD_OWN  (1u << 13)  /* Set by the NIC when a TX slot is free   */

#define NUM_TX_DESC 4
#define TX_BUF_SIZE 1792        /* Comfortably above max Ethernet frame */
#define TX_MIN_FRAME 60         /* Ethernet minimum payload, pre-CRC    */

#define RX_BUF_WRAP_SIZE 8192   /* Nominal RX ring size (RCR's "8K")    */
#define RX_BUF_ALLOC_SIZE (RX_BUF_WRAP_SIZE + 16 + 1500) /* + overrun pad */

static uint16_t io_base = 0;
static uint8_t  nic_mac[6];
static uint8_t *rx_buffer = 0;
static uint32_t rx_offset = 0;
static uint8_t *tx_buf[NUM_TX_DESC];
static int      tx_next = 0;
static rtl8139_rx_callback_t g_rx_callback = 0;

static void rtl8139_receive_packets(void) {
    /* CMD.BUFE (bit 0) is set once the ring has no more complete
     * packets buffered - the standard "drain until empty" loop. */
    while (!(inb(io_base + REG_CMD) & CMD_BUFE)) {
        uint8_t *hdr = rx_buffer + rx_offset;
        uint16_t status = *(volatile uint16_t *)hdr;
        uint16_t size   = *(volatile uint16_t *)(hdr + 2);

        if (!(status & 0x01)) {
            /* Not ROK for this entry - ring is corrupt/desynced; stop
             * rather than risk an unbounded loop over garbage. */
            break;
        }

        uint16_t frame_len = (size >= 4) ? (uint16_t)(size - 4) : 0; /* drop trailing CRC */
        if (g_rx_callback && frame_len > 0 && frame_len <= 1514) {
            g_rx_callback(hdr + 4, frame_len);
        }

        /* Advance past this entry (4-byte header + data), 4-byte
         * aligned, wrapping at the nominal 8 KiB ring boundary - the
         * canonical RTL8139 receive-advance formula. */
        rx_offset = (rx_offset + size + 4 + 3) & ~3u;
        if (rx_offset > RX_BUF_WRAP_SIZE) {
            rx_offset -= RX_BUF_WRAP_SIZE;
        }

        /* CAPR is written as (offset - 16): the chip's documented
         * off-by-16 convention, to keep its internal prefetch from
         * running into the entry currently being read. */
        outw(io_base + REG_CAPR, (uint16_t)(rx_offset - 16));
    }
}

static void rtl8139_irq_handler(void) {
    uint16_t status = inw(io_base + REG_ISR);
    outw(io_base + REG_ISR, status);   /* write-1-to-clear, ack immediately */

    if (status & ISR_ROK) {
        rtl8139_receive_packets();
    }
    /* TOK/TER/RER/RXOVW: nothing further to do for this minimal driver -
     * rtl8139_send() doesn't block on completion, and errors are rare
     * enough in a QEMU user-mode-networking setup not to need recovery
     * logic here. */
}

int rtl8139_init(rtl8139_rx_callback_t rx_callback) {
    pci_device_t dev;
    if (pci_find_device(0x10EC, 0x8139, &dev) != 0) {
        console_write("RTL8139: no device found on PCI bus\n");
        return -1;
    }

    if (!(dev.bar[0] & 0x1)) {
        console_write("RTL8139: BAR0 is not an I/O-space BAR\n");
        return -1;
    }
    io_base = (uint16_t)(dev.bar[0] & ~0x3u);

    pci_enable_bus_mastering(&dev);

    outb(io_base + REG_CONFIG1, 0x00);   /* Power on */

    outb(io_base + REG_CMD, CMD_RST);    /* Software reset */
    {
        int timeout = 1000000;
        while ((inb(io_base + REG_CMD) & CMD_RST) && timeout-- > 0) {
            /* spin */
        }
        if (timeout <= 0) {
            console_write("RTL8139: reset timed out\n");
            return -1;
        }
    }

    for (int i = 0; i < 6; i++) {
        nic_mac[i] = inb(io_base + REG_MAC0 + i);
    }

    rx_buffer = (uint8_t *)kmalloc(RX_BUF_ALLOC_SIZE);
    if (!rx_buffer) {
        console_write("RTL8139: out of memory for RX buffer\n");
        return -1;
    }
    memset(rx_buffer, 0, RX_BUF_ALLOC_SIZE);
    rx_offset = 0;
    /* Identity-mapped kernel heap: this virtual address IS the physical
     * address the NIC's DMA engine needs. */
    outl(io_base + REG_RBSTART, (uint32_t)(uintptr_t)rx_buffer);

    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_buf[i] = (uint8_t *)kmalloc(TX_BUF_SIZE);
        if (!tx_buf[i]) {
            console_write("RTL8139: out of memory for TX buffer\n");
            return -1;
        }
        outl(io_base + REG_TSAD0 + i * 4, (uint32_t)(uintptr_t)tx_buf[i]);
    }
    tx_next = 0;

    outw(io_base + REG_IMR, ISR_ROK | ISR_TOK);

    /* RCR: accept broadcast (AB) + multicast (AM) + physical-match (APM)
     * + all (AAP, keeps ARP/DHCP-style traffic flowing without needing
     * exact multicast filtering), WRAP=1 (bit 7, matches the RX_BUF_*
     * sizing/advance logic above), 8 KiB+16 ring (RBLEN bits left at 00). */
    outl(io_base + REG_RCR, 0x0Fu | (1u << 7));

    outb(io_base + REG_CMD, CMD_RE | CMD_TE);   /* Enable RX and TX */

    g_rx_callback = rx_callback;
    irq_install_handler(dev.irq_line, rtl8139_irq_handler);

    console_write("RTL8139: found at PCI ");
    {
        char buf[12];
        itoa(dev.slot, buf, 10);
        console_write("slot ");
        console_write(buf);
    }
    console_write(", io=0x");
    {
        char buf[12];
        itoa(io_base, buf, 16);
        console_write(buf);
    }
    console_write(", irq=");
    {
        char buf[12];
        itoa(dev.irq_line, buf, 10);
        console_write(buf);
    }
    console_write(", MAC=");
    for (int i = 0; i < 6; i++) {
        char buf[4];
        itoa(nic_mac[i], buf, 16);
        if (nic_mac[i] < 0x10) console_write("0");
        console_write(buf);
        if (i < 5) console_write(":");
    }
    console_write("\n");

    return 0;
}

void rtl8139_get_mac(uint8_t out_mac[6]) {
    memcpy(out_mac, nic_mac, 6);
}

int rtl8139_send(const void *data, uint16_t len) {
    if (!data || len == 0 || len > TX_BUF_SIZE) {
        return -1;
    }

    int slot = tx_next;
    uint32_t tsd_reg = REG_TSD0 + (uint32_t)slot * 4;

    if (!(inl(io_base + tsd_reg) & TSD_OWN)) {
        /* Still transmitting the previous frame from this slot. With
         * only 4 slots and no queuing, the caller simply loses this
         * send - acceptable for the demo-scale traffic this stack
         * handles (ARP/ICMP/small UDP datagrams), not a bulk-throughput
         * NIC path. */
        return -1;
    }

    memcpy(tx_buf[slot], data, len);

    uint16_t send_len = len;
    if (send_len < TX_MIN_FRAME) {
        memset(tx_buf[slot] + len, 0, TX_MIN_FRAME - len);
        send_len = TX_MIN_FRAME;
    }

    outl(io_base + tsd_reg, send_len & 0x1FFFu);
    tx_next = (tx_next + 1) % NUM_TX_DESC;

    return (int)len;
}
