/*
 * OrgitoOS - Networking Stack
 *
 * Ethernet/ARP/IP/ICMP/UDP over a real RTL8139 NIC (drivers/rtl8139.c).
 * Provides basic TCP/IP networking capabilities.
 */

#ifndef ORGITOOS_INCLUDE_NETWORK_H
#define ORGITOOS_INCLUDE_NETWORK_H

#include <stdint.h>
#include <stddef.h>

/* MAC address length */
#define MAC_ADDR_LEN 6

/* IP address length */
#define IP_ADDR_LEN 4

/* Maximum packet size */
#define MAX_PACKET_SIZE 1518

/* Protocol numbers */
#define PROTO_ICMP 1
#define PROTO_TCP  6
#define PROTO_UDP  17

/* EtherType values */
#define ETHERTYPE_IP  0x0800
#define ETHERTYPE_ARP 0x0806

/* MAC address structure */
typedef struct mac_addr {
    uint8_t addr[MAC_ADDR_LEN];
} mac_addr_t;

/* IP address structure */
typedef struct ip_addr {
    uint8_t addr[IP_ADDR_LEN];
} ip_addr_t;

/* Ethernet header (fields are big-endian/"network order" on the wire -
 * read/write `type` through net_htons()/net_ntohs(), never directly) */
typedef struct eth_header {
    mac_addr_t dest;
    mac_addr_t src;
    uint16_t type;
} __attribute__((packed)) eth_header_t;

/* ARP header (RFC 826, Ethernet/IPv4 case only) */
typedef struct arp_header {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;      /* 1 = request, 2 = reply */
    mac_addr_t sha;      /* sender hardware address */
    ip_addr_t  spa;      /* sender protocol address */
    mac_addr_t tha;      /* target hardware address */
    ip_addr_t  tpa;      /* target protocol address */
} __attribute__((packed)) arp_header_t;

/* IP header */
typedef struct ip_header {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_offset;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    ip_addr_t src_ip;
    ip_addr_t dst_ip;
} __attribute__((packed)) ip_header_t;

/* ICMP header (echo request/reply layout) */
typedef struct icmp_header {
    uint8_t  type;   /* 8 = echo request, 0 = echo reply */
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_header_t;

/* TCP header. `flags` packs the wire's 4-bit data-offset (header length in
 * 32-bit words) + 6 reserved bits + 6 flag bits into one big-endian 16-bit
 * field, per RFC 793 - use TCP_MAKE_OFFSET_FLAGS()/the TCP_FLAG_* bits
 * below rather than poking it directly. */
typedef struct tcp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_header_t;

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

/* Builds tcp_header_t.flags' pre-byte-order value: `doff_words` is the
 * header length in 32-bit words (5 for a plain 20-byte header, no
 * options), `flags` is any OR of TCP_FLAG_*. Pass the result through
 * net_htons() same as any other multi-byte field. */
#define TCP_MAKE_OFFSET_FLAGS(doff_words, flags) ((uint16_t)(((doff_words) << 12) | (flags)))

/* Minimal client-only TCP state machine (kernel/network.c handle_tcp()):
 * active-open connect, established data transfer, FIN close. No
 * listen()/accept() (no LISTEN/SYN_RCVD), no retransmission on loss, and
 * TCP_CLOSE_WAIT skips straight to CLOSED on our own close() rather than
 * tracking TIME_WAIT - all explicit, documented scope limits, not bugs. */
typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT
} tcp_state_t;

/* UDP header */
typedef struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_header_t;

/* Network packet structure */
typedef struct packet {
    uint8_t data[MAX_PACKET_SIZE];
    size_t length;
} packet_t;

/* Depth of a UDP/TCP socket's receive queue and max payload per queued
 * datagram/segment - a real (if small) buffer, populated by the RX
 * interrupt path (kernel/network.c handle_udp()/handle_tcp()) and drained
 * by net_socket_recv(). Also doubles as the max single-segment TCP send
 * size (this stack sends one segment per net_socket_send() call, no
 * fragmentation across multiple segments). */
#define SOCK_RX_QUEUE_LEN 4
#define SOCK_RX_MAX_LEN   512

/* Socket structure */
typedef struct socket {
    uint32_t id;
    uint16_t local_port;
    uint16_t remote_port;
    ip_addr_t remote_ip;
    uint8_t protocol;
    int is_open;

    /* TCP connection state (protocol == PROTO_TCP only; see tcp_state_t) */
    uint8_t  tcp_state;
    uint32_t snd_nxt;   /* next sequence number this side will send */
    uint32_t rcv_nxt;   /* next sequence number expected from the peer */

    /* UDP/TCP receive queue (protocol == PROTO_UDP or PROTO_TCP) */
    uint8_t   rx_queue[SOCK_RX_QUEUE_LEN][SOCK_RX_MAX_LEN];
    uint16_t  rx_len[SOCK_RX_QUEUE_LEN];
    ip_addr_t rx_src_ip[SOCK_RX_QUEUE_LEN];
    uint16_t  rx_src_port[SOCK_RX_QUEUE_LEN];
    uint8_t   rx_head;
    uint8_t   rx_count;
} socket_t;

/* Network device structure */
typedef struct net_device {
    char name[16];
    mac_addr_t mac;
    ip_addr_t ip;
    ip_addr_t netmask;
    ip_addr_t gateway;
    int is_up;
} net_device_t;

/* Initialize networking subsystem: PCI-probes for an RTL8139, and if
 * found, brings it up with a real MAC and installs the RX interrupt
 * path (kernel/network.c net_handle_frame). If no NIC is present,
 * the device stays down and every send/receive call below is a no-op -
 * network-dependent shell commands degrade gracefully rather than
 * hanging or crashing. */
void net_init(void);

/* Network device management */
void net_set_ip(ip_addr_t* ip);
void net_set_mac(mac_addr_t* mac);
net_device_t* net_get_device(void);

/* Byte-order helpers: every multi-byte field in the headers above is
 * big-endian on the wire; x86 is little-endian, so any read/write of
 * `type`/`total_length`/`checksum`/etc. must go through these. */
uint16_t net_htons(uint16_t v);
uint16_t net_ntohs(uint16_t v);
uint32_t net_htonl(uint32_t v);
uint32_t net_ntohl(uint32_t v);

/* Packet operations. net_send_packet() really transmits via the NIC
 * driver. net_receive_packet() is a legacy polling-style API that
 * predates the interrupt-driven RX path below (arriving frames are now
 * dispatched automatically to ARP/ICMP/UDP as they arrive, not queued
 * here) - it always returns 0 and is kept only for source
 * compatibility. */
int net_send_packet(packet_t* packet);
int net_receive_packet(packet_t* packet);

/* Resolve an IP address to a MAC address via ARP, using the cache if
 * possible and otherwise broadcasting a request and blocking (with
 * interrupts enabled, so the reply can actually be processed) for up
 * to ~1 second. Returns 0 and fills *out_mac on success, -1 on
 * timeout/no NIC. */
int net_arp_resolve(const ip_addr_t* ip, mac_addr_t* out_mac);

/* Send an ICMP echo request to `target_ip` and block for a reply (or
 * timeout, ~1s). Returns 0 and fills *out_rtt_ms on success, -1 on
 * failure (ARP timeout, send failure, or no reply). Used by the `ping`
 * shell command (kernel/commands.c). */
int net_ping(const ip_addr_t* target_ip, uint32_t* out_rtt_ms);

/* Socket operations. PROTO_UDP sends/receives real datagrams; PROTO_TCP
 * runs a minimal client-only state machine (active-open connect() does a
 * real 3-way handshake and blocks for it, send()/recv() work on an
 * established connection, close() sends FIN) - see tcp_state_t's doc
 * comment for the explicit scope limits (no listen/accept, no
 * retransmission). Other protocol values behave as inert placeholders. */
socket_t* net_socket_create(uint8_t protocol);
int net_socket_bind(socket_t* socket, uint16_t port);
int net_socket_connect(socket_t* socket, ip_addr_t* ip, uint16_t port);
int net_socket_send(socket_t* socket, const void* data, size_t size);
int net_socket_recv(socket_t* socket, void* buffer, size_t size);
void net_socket_close(socket_t* socket);

/* Utility functions */
uint16_t net_checksum(const void* data, size_t length);

#endif /* ORGITOOS_INCLUDE_NETWORK_H */
