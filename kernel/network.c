/*
 * OrgitoOS - Networking Implementation
 *
 * Real Ethernet/ARP/IP/ICMP/UDP over drivers/rtl8139.c, replacing the
 * former stubs: net_send_packet() really transmits, incoming frames are
 * dispatched from the NIC's IRQ handler through net_handle_frame() below
 * to real ARP-cache updates, ICMP echo request/reply, and per-socket UDP
 * receive queues, and net_socket_send()/net_socket_recv() build and
 * parse real on-wire packets instead of returning `size` unchanged.
 */

#include "network.h"
#include "console.h"
#include "string.h"
#include "../drivers/rtl8139.h"
#include "../drivers/timer.h"
#include "../memory/heap.h"

/* Global network device */
static net_device_t net_dev;
static int net_initialized = 0;
static int nic_present = 0;

/* Socket storage */
#define MAX_SOCKETS 32
static socket_t sockets[MAX_SOCKETS];

/* ARP cache: small, linear, good enough for a handful of peers (the
 * default gateway, maybe one or two hosts a demo talks to). */
#define ARP_CACHE_SIZE 16
typedef struct {
    ip_addr_t ip;
    mac_addr_t mac;
    int valid;
} arp_entry_t;
static arp_entry_t arp_cache[ARP_CACHE_SIZE];

/* Pending ping state (single outstanding ping at a time - matches the
 * `ping` shell command's one-shot usage). */
static volatile int ping_reply_received = 0;
static uint16_t ping_expected_id = 0;
static uint16_t ping_expected_seq = 0;
static ip_addr_t ping_expected_src;

/* ------------------------------------------------------------------ */
/* Byte order                                                           */
/* ------------------------------------------------------------------ */

uint16_t net_htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
uint16_t net_ntohs(uint16_t v) { return net_htons(v); }

uint32_t net_htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
uint32_t net_ntohl(uint32_t v) { return net_htonl(v); }

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

static int ip_equal(const ip_addr_t *a, const ip_addr_t *b) {
    return memcmp(a->addr, b->addr, 4) == 0;
}

static int ip_same_subnet(const ip_addr_t *a, const ip_addr_t *b, const ip_addr_t *mask) {
    for (int i = 0; i < 4; i++) {
        if ((a->addr[i] & mask->addr[i]) != (b->addr[i] & mask->addr[i])) {
            return 0;
        }
    }
    return 1;
}

static ip_addr_t route_next_hop(const ip_addr_t *dst) {
    if (ip_same_subnet(dst, &net_dev.ip, &net_dev.netmask)) {
        return *dst;
    }
    return net_dev.gateway;
}

static void arp_cache_update(const ip_addr_t *ip, const mac_addr_t *mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ip_equal(&arp_cache[i].ip, ip)) {
            arp_cache[i].mac = *mac;
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = *ip;
            arp_cache[i].mac = *mac;
            arp_cache[i].valid = 1;
            return;
        }
    }
    /* Cache full: overwrite slot 0 rather than dropping the update -
     * a tiny fixed-size LRU-less cache is plenty for this stack's scale. */
    arp_cache[0].ip = *ip;
    arp_cache[0].mac = *mac;
    arp_cache[0].valid = 1;
}

static int arp_cache_lookup(const ip_addr_t *ip, mac_addr_t *out_mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ip_equal(&arp_cache[i].ip, ip)) {
            *out_mac = arp_cache[i].mac;
            return 0;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* ARP                                                                   */
/* ------------------------------------------------------------------ */

static void send_arp_request(const ip_addr_t *target_ip) {
    if (!nic_present) return;

    uint8_t frame[sizeof(eth_header_t) + sizeof(arp_header_t)];
    eth_header_t *eth = (eth_header_t *)frame;
    memset(eth->dest.addr, 0xFF, 6);
    memcpy(eth->src.addr, net_dev.mac.addr, 6);
    eth->type = net_htons(ETHERTYPE_ARP);

    arp_header_t *arp = (arp_header_t *)(frame + sizeof(eth_header_t));
    arp->htype = net_htons(1);
    arp->ptype = net_htons(ETHERTYPE_IP);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = net_htons(1);
    memcpy(arp->sha.addr, net_dev.mac.addr, 6);
    memcpy(arp->spa.addr, net_dev.ip.addr, 4);
    memset(arp->tha.addr, 0, 6);
    memcpy(arp->tpa.addr, target_ip->addr, 4);

    rtl8139_send(frame, sizeof(frame));
}

static void send_arp_reply(const ip_addr_t *target_ip, const mac_addr_t *target_mac) {
    if (!nic_present) return;

    uint8_t frame[sizeof(eth_header_t) + sizeof(arp_header_t)];
    eth_header_t *eth = (eth_header_t *)frame;
    eth->dest = *target_mac;
    memcpy(eth->src.addr, net_dev.mac.addr, 6);
    eth->type = net_htons(ETHERTYPE_ARP);

    arp_header_t *arp = (arp_header_t *)(frame + sizeof(eth_header_t));
    arp->htype = net_htons(1);
    arp->ptype = net_htons(ETHERTYPE_IP);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = net_htons(2);
    memcpy(arp->sha.addr, net_dev.mac.addr, 6);
    memcpy(arp->spa.addr, net_dev.ip.addr, 4);
    arp->tha = *target_mac;
    memcpy(arp->tpa.addr, target_ip->addr, 4);

    rtl8139_send(frame, sizeof(frame));
}

int net_arp_resolve(const ip_addr_t *ip, mac_addr_t *out_mac) {
    if (!nic_present) return -1;
    if (arp_cache_lookup(ip, out_mac) == 0) return 0;

    send_arp_request(ip);

    uint64_t deadline = timer_get_ticks() + 100;   /* ~1s @ 100 Hz */
    while (timer_get_ticks() < deadline) {
        if (arp_cache_lookup(ip, out_mac) == 0) return 0;
        __asm__ __volatile__("pause");
    }
    return -1;
}

static void handle_arp(const uint8_t *data, uint16_t len) {
    if (len < sizeof(arp_header_t)) return;
    const arp_header_t *arp = (const arp_header_t *)data;

    if (net_ntohs(arp->htype) != 1 || net_ntohs(arp->ptype) != ETHERTYPE_IP) {
        return;
    }

    /* Learn the sender's mapping regardless of request/reply - standard
     * ARP cache behavior, and what lets a reply to our own request
     * (sent from send_arp_request above) actually get recorded. */
    arp_cache_update(&arp->spa, &arp->sha);

    if (net_ntohs(arp->oper) == 1 && ip_equal(&arp->tpa, &net_dev.ip)) {
        send_arp_reply(&arp->spa, &arp->sha);
    }
}

/* ------------------------------------------------------------------ */
/* IP / ICMP / UDP send helpers                                         */
/* ------------------------------------------------------------------ */

static uint16_t ip_id_counter = 1;

/* Fills an IP header (with a correct checksum) at `hdr`, for a payload
 * of `payload_len` bytes and the given protocol/destination. */
static void build_ip_header(ip_header_t *hdr, uint8_t protocol, const ip_addr_t *dst, uint16_t payload_len) {
    hdr->version_ihl = 0x45;   /* IPv4, 5 * 4 = 20-byte header, no options */
    hdr->tos = 0;
    hdr->total_length = net_htons((uint16_t)(sizeof(ip_header_t) + payload_len));
    hdr->id = net_htons(ip_id_counter++);
    hdr->flags_offset = 0;
    hdr->ttl = 64;
    hdr->protocol = protocol;
    hdr->checksum = 0;
    memcpy(hdr->src_ip.addr, net_dev.ip.addr, 4);
    memcpy(hdr->dst_ip.addr, dst->addr, 4);
    /* checksum is already 0 above - computed over the struct with the
     * field zeroed, the standard IP/ICMP checksum convention. Assigned
     * by value (not through &hdr->checksum) since this is a packed
     * struct and taking a packed member's address risks an unaligned
     * pointer on stricter architectures - harmless on x86, but avoided
     * for cleanliness. */
    hdr->checksum = net_htons(net_checksum(hdr, sizeof(ip_header_t)));
}

/*
 * Sends an Ethernet+IP+payload frame to `dst_ip`, resolving its MAC (or
 * the gateway's, if off-subnet) first. Returns bytes sent, or -1 on ARP
 * failure/no NIC/oversized payload.
 *
 * `allow_arp_wait` MUST be 0 for any call reachable from IRQ context
 * (the RTL8139 IRQ handler -> net_handle_frame -> ... call chain):
 * net_arp_resolve()'s wait loop polls timer_get_ticks(), but the timer
 * IRQ that advances it can never fire while this interrupt gate has
 * IF=0 (arch/x86/idt.h's IDT_FLAGS_KERNEL), which would deadlock the
 * whole system instead of merely timing out. With allow_arp_wait=0,
 * only an existing cache entry is used - see net_handle_frame's
 * opportunistic ARP-cache learning from every received IP packet's
 * source MAC, which is what keeps the one IRQ-context caller here
 * (handle_icmp's echo-reply path) a guaranteed cache hit in practice. */
static int send_ip_packet(const ip_addr_t *dst_ip, uint8_t protocol,
                          const void *payload, uint16_t payload_len, int allow_arp_wait) {
    if (!nic_present) return -1;
    if (sizeof(eth_header_t) + sizeof(ip_header_t) + payload_len > MAX_PACKET_SIZE) return -1;

    mac_addr_t dst_mac;
    ip_addr_t next_hop = route_next_hop(dst_ip);
    int resolved;
    if (allow_arp_wait) {
        resolved = net_arp_resolve(&next_hop, &dst_mac);
    } else {
        resolved = arp_cache_lookup(&next_hop, &dst_mac);
    }
    if (resolved != 0) {
        return -1;
    }

    uint8_t frame[MAX_PACKET_SIZE];
    eth_header_t *eth = (eth_header_t *)frame;
    eth->dest = dst_mac;
    memcpy(eth->src.addr, net_dev.mac.addr, 6);
    eth->type = net_htons(ETHERTYPE_IP);

    ip_header_t *ip = (ip_header_t *)(frame + sizeof(eth_header_t));
    build_ip_header(ip, protocol, dst_ip, payload_len);

    memcpy(frame + sizeof(eth_header_t) + sizeof(ip_header_t), payload, payload_len);

    uint16_t total = (uint16_t)(sizeof(eth_header_t) + sizeof(ip_header_t) + payload_len);
    return rtl8139_send(frame, total);
}

static void send_icmp(const ip_addr_t *dst_ip, uint8_t type, uint8_t code,
                      uint16_t id, uint16_t seq, const uint8_t *payload, uint16_t payload_len,
                      int allow_arp_wait) {
    uint8_t buf[sizeof(icmp_header_t) + 64];
    if (payload_len > 64) payload_len = 64;

    icmp_header_t *icmp = (icmp_header_t *)buf;
    icmp->type = type;
    icmp->code = code;
    icmp->checksum = 0;
    icmp->id = net_htons(id);
    icmp->seq = net_htons(seq);
    if (payload && payload_len) {
        memcpy(buf + sizeof(icmp_header_t), payload, payload_len);
    }

    uint16_t total_len = (uint16_t)(sizeof(icmp_header_t) + payload_len);
    /* checksum field is already 0 above - see build_ip_header()'s comment
     * on why this is assigned by value rather than through a pointer. */
    icmp->checksum = net_htons(net_checksum(buf, total_len));

    send_ip_packet(dst_ip, PROTO_ICMP, buf, total_len, allow_arp_wait);
}

/* Runs in IRQ context (net_handle_frame -> handle_ip -> here) - must
 * never block. The echo-reply send below passes allow_arp_wait=0. */
static void handle_icmp(const ip_addr_t *src_ip, const uint8_t *data, uint16_t len) {
    if (len < sizeof(icmp_header_t)) return;
    const icmp_header_t *icmp = (const icmp_header_t *)data;
    const uint8_t *payload = data + sizeof(icmp_header_t);
    uint16_t payload_len = (uint16_t)(len - sizeof(icmp_header_t));

    if (icmp->type == 8) {
        /* Echo request -> reply with the same id/seq/payload. Relies on
         * net_handle_frame() having already warmed the ARP cache for
         * src_ip from this same frame's Ethernet source MAC. */
        send_icmp(src_ip, 0, 0, net_ntohs(icmp->id), net_ntohs(icmp->seq), payload, payload_len, 0);
    } else if (icmp->type == 0) {
        /* Echo reply -> satisfy a pending net_ping(), if this is it. */
        if (net_ntohs(icmp->id) == ping_expected_id &&
            net_ntohs(icmp->seq) == ping_expected_seq &&
            ip_equal(src_ip, &ping_expected_src)) {
            ping_reply_received = 1;
        }
    }
}

int net_ping(const ip_addr_t *target_ip, uint32_t *out_rtt_ms) {
    if (!nic_present) return -1;

    mac_addr_t dst_mac;
    ip_addr_t next_hop = route_next_hop(target_ip);
    if (net_arp_resolve(&next_hop, &dst_mac) != 0) {
        return -1;
    }

    static uint16_t seq_counter = 0;
    uint16_t id = 0xACE0;
    uint16_t seq = ++seq_counter;

    ping_expected_id = id;
    ping_expected_seq = seq;
    ping_expected_src = *target_ip;
    ping_reply_received = 0;

    const char pattern[16] = "OrgitoOS-ping!!";
    uint64_t t0 = timer_get_ticks();
    send_icmp(target_ip, 8, 0, id, seq, (const uint8_t *)pattern, sizeof(pattern), 1);

    uint64_t deadline = t0 + 100;   /* ~1s @ 100 Hz */
    while (timer_get_ticks() < deadline) {
        if (ping_reply_received) {
            if (out_rtt_ms) *out_rtt_ms = (uint32_t)((timer_get_ticks() - t0) * 10);
            return 0;
        }
        __asm__ __volatile__("pause");
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* UDP                                                                   */
/* ------------------------------------------------------------------ */

static void handle_udp(const ip_addr_t *src_ip, const uint8_t *data, uint16_t len) {
    if (len < sizeof(udp_header_t)) return;
    const udp_header_t *udp = (const udp_header_t *)data;

    uint16_t dst_port = net_ntohs(udp->dst_port);
    uint16_t src_port = net_ntohs(udp->src_port);
    const uint8_t *payload = data + sizeof(udp_header_t);
    uint16_t payload_len = (uint16_t)(len - sizeof(udp_header_t));
    if (payload_len > SOCK_RX_MAX_LEN) payload_len = SOCK_RX_MAX_LEN;

    for (int i = 0; i < MAX_SOCKETS; i++) {
        socket_t *s = &sockets[i];
        if (!s->is_open || s->protocol != PROTO_UDP || s->local_port != dst_port) {
            continue;
        }
        if (s->rx_count >= SOCK_RX_QUEUE_LEN) {
            break;   /* Queue full - drop, matches a full socket recv buffer */
        }
        uint8_t slot = (uint8_t)((s->rx_head + s->rx_count) % SOCK_RX_QUEUE_LEN);
        memcpy(s->rx_queue[slot], payload, payload_len);
        s->rx_len[slot] = payload_len;
        s->rx_src_ip[slot] = *src_ip;
        s->rx_src_port[slot] = src_port;
        s->rx_count++;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* TCP - minimal client-only state machine (see tcp_state_t)            */
/* ------------------------------------------------------------------ */

/* Room for a pseudo-header + a full header + one max-size payload -
 * sized to match the largest buffer send_tcp_segment() ever checksums. */
#define TCP_PSEUDO_HDR_LEN 12
#define TCP_MAX_PAYLOAD SOCK_RX_MAX_LEN

/*
 * TCP's checksum (unlike UDP's, which is optional) covers a 12-byte
 * pseudo-header (src/dst IP, zero, protocol, TCP length) in addition to
 * the segment itself, per RFC 793 - real TCP stacks reject a segment
 * whose checksum doesn't account for it. Builds pseudo-header + segment
 * into a scratch buffer and reuses net_checksum() over the whole thing,
 * same one-'s-complement algorithm IP/ICMP already use.
 */
static uint16_t compute_tcp_checksum(const ip_addr_t *src, const ip_addr_t *dst,
                                     const void *segment, uint16_t segment_len) {
    uint8_t buf[TCP_PSEUDO_HDR_LEN + sizeof(tcp_header_t) + TCP_MAX_PAYLOAD];
    uint8_t *p = buf;

    memcpy(p, src->addr, 4); p += 4;
    memcpy(p, dst->addr, 4); p += 4;
    *p++ = 0;
    *p++ = PROTO_TCP;
    uint16_t len_be = net_htons(segment_len);
    memcpy(p, &len_be, 2); p += 2;

    memcpy(p, segment, segment_len);

    return net_htons(net_checksum(buf, (size_t)TCP_PSEUDO_HDR_LEN + segment_len));
}

/*
 * Builds and sends one TCP segment for `s`, then advances s->snd_nxt by
 * however many sequence numbers this segment consumes (SYN and FIN each
 * consume one, even with no payload, per RFC 793). `allow_arp_wait` MUST
 * be 0 when called from IRQ context (handle_tcp(), below, replying to a
 * just-received segment) - see send_ip_packet()'s doc comment - and is
 * safe to pass 1 from ordinary process/shell context (net_socket_connect/
 * send/close). Returns 0 on success, -1 on failure (no NIC, ARP failure,
 * oversized payload).
 */
static int send_tcp_segment(socket_t *s, uint8_t flags, const uint8_t *payload,
                            uint16_t payload_len, int allow_arp_wait) {
    if (!nic_present) return -1;
    if (payload_len > TCP_MAX_PAYLOAD) return -1;

    uint8_t buf[sizeof(tcp_header_t) + TCP_MAX_PAYLOAD];
    tcp_header_t *tcp = (tcp_header_t *)buf;
    tcp->src_port = net_htons(s->local_port);
    tcp->dst_port = net_htons(s->remote_port);
    tcp->seq_num  = net_htonl(s->snd_nxt);
    tcp->ack_num  = net_htonl(s->rcv_nxt);
    tcp->flags    = net_htons(TCP_MAKE_OFFSET_FLAGS(5, flags));
    tcp->window   = net_htons(8192);
    tcp->checksum = 0;
    tcp->urgent   = 0;
    if (payload && payload_len) {
        memcpy(buf + sizeof(tcp_header_t), payload, payload_len);
    }

    uint16_t total_len = (uint16_t)(sizeof(tcp_header_t) + payload_len);
    /* checksum field is already 0 above - see build_ip_header()'s comment
     * on why this is assigned by value rather than through a pointer. */
    tcp->checksum = compute_tcp_checksum(&net_dev.ip, &s->remote_ip, buf, total_len);

    if (send_ip_packet(&s->remote_ip, PROTO_TCP, buf, total_len, allow_arp_wait) < 0) {
        return -1;
    }

    uint32_t consumed = payload_len;
    if (flags & TCP_FLAG_SYN) consumed++;
    if (flags & TCP_FLAG_FIN) consumed++;
    s->snd_nxt += consumed;
    return 0;
}

/* Runs in IRQ context (net_handle_frame -> handle_ip -> here) - must
 * never block, so every reply below passes allow_arp_wait=0 (safe: the
 * ARP cache was already warmed for this peer either by the 3-way
 * handshake's own send_tcp_segment() call, or opportunistically by
 * net_handle_frame(), same as handle_icmp()'s echo-reply path). */
static void handle_tcp(const ip_addr_t *src_ip, const uint8_t *data, uint16_t len) {
    if (len < sizeof(tcp_header_t)) return;
    const tcp_header_t *tcp = (const tcp_header_t *)data;

    uint16_t dst_port = net_ntohs(tcp->dst_port);
    uint16_t src_port = net_ntohs(tcp->src_port);
    uint16_t offset_flags = net_ntohs(tcp->flags);
    uint8_t  flags = (uint8_t)(offset_flags & 0x3F);
    uint16_t hdr_len = (uint16_t)((offset_flags >> 12) * 4);
    if (hdr_len < sizeof(tcp_header_t) || hdr_len > len) return;

    const uint8_t *payload = data + hdr_len;
    uint16_t payload_len = (uint16_t)(len - hdr_len);
    if (payload_len > SOCK_RX_MAX_LEN) payload_len = SOCK_RX_MAX_LEN;
    uint32_t seq = net_ntohl(tcp->seq_num);
    uint32_t ack = net_ntohl(tcp->ack_num);

    for (int i = 0; i < MAX_SOCKETS; i++) {
        socket_t *s = &sockets[i];
        if (!s->is_open || s->protocol != PROTO_TCP || s->local_port != dst_port) {
            continue;
        }
        /* Once a peer is set (post-connect), only accept segments from it -
         * this stack has no listen()/accept(), so every TCP socket has
         * exactly one peer for its whole lifetime. */
        if (s->remote_port != src_port || !ip_equal(src_ip, &s->remote_ip)) {
            continue;
        }

        if (flags & TCP_FLAG_RST) {
            s->tcp_state = TCP_CLOSED;
            break;
        }

        switch (s->tcp_state) {
            case TCP_SYN_SENT:
                if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK) && ack == s->snd_nxt) {
                    s->rcv_nxt = seq + 1;
                    s->tcp_state = TCP_ESTABLISHED;
                    send_tcp_segment(s, TCP_FLAG_ACK, 0, 0, 0);
                }
                break;

            case TCP_ESTABLISHED:
                if (payload_len > 0 && seq == s->rcv_nxt) {
                    if (s->rx_count < SOCK_RX_QUEUE_LEN) {
                        uint8_t slot = (uint8_t)((s->rx_head + s->rx_count) % SOCK_RX_QUEUE_LEN);
                        memcpy(s->rx_queue[slot], payload, payload_len);
                        s->rx_len[slot] = payload_len;
                        s->rx_src_ip[slot] = *src_ip;
                        s->rx_src_port[slot] = src_port;
                        s->rx_count++;
                    }
                    s->rcv_nxt = seq + payload_len;
                    send_tcp_segment(s, TCP_FLAG_ACK, 0, 0, 0);
                }
                if (flags & TCP_FLAG_FIN) {
                    s->rcv_nxt = seq + payload_len + 1;
                    send_tcp_segment(s, TCP_FLAG_ACK, 0, 0, 0);
                    s->tcp_state = TCP_CLOSE_WAIT;
                }
                break;

            case TCP_FIN_WAIT1:
                if ((flags & TCP_FLAG_ACK) && ack == s->snd_nxt) {
                    s->tcp_state = TCP_FIN_WAIT2;
                }
                if (flags & TCP_FLAG_FIN) {
                    s->rcv_nxt = seq + 1;
                    send_tcp_segment(s, TCP_FLAG_ACK, 0, 0, 0);
                    s->tcp_state = TCP_CLOSED;
                }
                break;

            case TCP_FIN_WAIT2:
                if (flags & TCP_FLAG_FIN) {
                    s->rcv_nxt = seq + 1;
                    send_tcp_segment(s, TCP_FLAG_ACK, 0, 0, 0);
                    s->tcp_state = TCP_CLOSED;
                }
                break;

            default:
                break;
        }
        break;   /* matched exactly one socket by (local_port, remote peer) */
    }
}

/* ------------------------------------------------------------------ */
/* IP dispatch                                                          */
/* ------------------------------------------------------------------ */

static void handle_ip(const uint8_t *data, uint16_t len) {
    if (len < sizeof(ip_header_t)) return;
    const ip_header_t *ip = (const ip_header_t *)data;

    uint8_t ihl = (uint8_t)((ip->version_ihl & 0x0F) * 4);
    if (ihl < sizeof(ip_header_t) || len < ihl) return;
    if (!ip_equal(&ip->dst_ip, &net_dev.ip)) return;   /* not addressed to us */

    uint16_t total_len = net_ntohs(ip->total_length);
    if (total_len < ihl || total_len > len) total_len = len;   /* defensive clamp */

    const uint8_t *l4 = data + ihl;
    uint16_t l4_len = (uint16_t)(total_len - ihl);

    if (ip->protocol == PROTO_ICMP) {
        handle_icmp(&ip->src_ip, l4, l4_len);
    } else if (ip->protocol == PROTO_UDP) {
        handle_udp(&ip->src_ip, l4, l4_len);
    } else if (ip->protocol == PROTO_TCP) {
        handle_tcp(&ip->src_ip, l4, l4_len);
    }
}

/* ------------------------------------------------------------------ */
/* RX entry point - registered with the NIC driver, runs in IRQ context */
/* ------------------------------------------------------------------ */

static void net_handle_frame(const uint8_t *frame, uint16_t len) {
    if (len < sizeof(eth_header_t)) return;
    const eth_header_t *eth = (const eth_header_t *)frame;
    uint16_t ethertype = net_ntohs(eth->type);

    const uint8_t *payload = frame + sizeof(eth_header_t);
    uint16_t payload_len = (uint16_t)(len - sizeof(eth_header_t));

    if (ethertype == ETHERTYPE_ARP) {
        handle_arp(payload, payload_len);
    } else if (ethertype == ETHERTYPE_IP) {
        /* Opportunistically learn the sender's IP -> MAC mapping from
         * data traffic too, not just ARP packets. This is what makes
         * handle_icmp()'s echo-reply send (which runs right here in IRQ
         * context and must never block on ARP) a guaranteed cache hit:
         * by the time handle_ip() -> handle_icmp() looks the sender up,
         * this same frame already populated it, a few lines up. */
        if (payload_len >= sizeof(ip_header_t)) {
            const ip_header_t *ip = (const ip_header_t *)payload;
            arp_cache_update(&ip->src_ip, &eth->src);
        }
        handle_ip(payload, payload_len);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                            */
/* ------------------------------------------------------------------ */

void net_init(void) {
    if (net_initialized) return;

    console_write("NET: Initializing networking stack...\n");

    strncpy(net_dev.name, "eth0", sizeof(net_dev.name));
    net_dev.is_up = 0;

    for (int i = 0; i < MAX_SOCKETS; i++) {
        sockets[i].is_open = 0;
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_cache[i].valid = 0;
    }

    /*
     * Defaults match QEMU's `-netdev user` (SLIRP) built-in network:
     * guest 10.0.2.15/24, gateway/DNS-forwarder 10.0.2.2. Overridable
     * via net_set_ip() before/after net_init() if the NIC is bridged to
     * something else instead.
     */
    net_dev.ip.addr[0] = 10;  net_dev.ip.addr[1] = 0; net_dev.ip.addr[2] = 2;  net_dev.ip.addr[3] = 15;
    net_dev.netmask.addr[0] = 255; net_dev.netmask.addr[1] = 255; net_dev.netmask.addr[2] = 255; net_dev.netmask.addr[3] = 0;
    net_dev.gateway.addr[0] = 10; net_dev.gateway.addr[1] = 0; net_dev.gateway.addr[2] = 2; net_dev.gateway.addr[3] = 2;

    if (rtl8139_init(net_handle_frame) == 0) {
        rtl8139_get_mac(net_dev.mac.addr);
        net_dev.is_up = 1;
        nic_present = 1;

        console_write("NET: eth0 up, IP ");
        char buf[16];
        for (int i = 0; i < 4; i++) {
            itoa(net_dev.ip.addr[i], buf, 10);
            console_write(buf);
            if (i < 3) console_write(".");
        }
        console_write("\n");
    } else {
        /* No RTL8139 present (e.g. QEMU launched without -device
         * rtl8139) - keep a synthetic MAC so net_get_device() callers
         * still see sane-looking data, but is_up stays 0 and every
         * send/receive path above short-circuits on nic_present. */
        net_dev.mac.addr[0] = 0x00; net_dev.mac.addr[1] = 0x11; net_dev.mac.addr[2] = 0x22;
        net_dev.mac.addr[3] = 0x33; net_dev.mac.addr[4] = 0x44; net_dev.mac.addr[5] = 0x55;
        console_write("NET: no NIC found - networking stays down\n");
        console_write("NET: QEMU users: add -device rtl8139,netdev=n0 -netdev user,id=n0\n");
    }

    net_initialized = 1;
}

void net_set_ip(ip_addr_t* ip) {
    if (ip) {
        memcpy(&net_dev.ip, ip, sizeof(ip_addr_t));
    }
}

void net_set_mac(mac_addr_t* mac) {
    if (mac) {
        memcpy(&net_dev.mac, mac, sizeof(mac_addr_t));
    }
}

net_device_t* net_get_device(void) {
    return &net_dev;
}

int net_send_packet(packet_t* packet) {
    if (!packet || !net_dev.is_up || !nic_present) return -1;
    return rtl8139_send(packet->data, (uint16_t)packet->length);
}

int net_receive_packet(packet_t* packet) {
    /* Superseded by the interrupt-driven net_handle_frame() path above -
     * see the doc comment in include/network.h. */
    (void)packet;
    return 0;
}

socket_t* net_socket_create(uint8_t protocol) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].is_open) {
            socket_t *s = &sockets[i];
            s->id = (uint32_t)i;
            s->protocol = protocol;
            s->local_port = 0;
            s->remote_port = 0;
            s->tcp_state = TCP_CLOSED;
            s->snd_nxt = 0;
            s->rcv_nxt = 0;
            s->rx_head = 0;
            s->rx_count = 0;
            s->is_open = 1;
            return s;
        }
    }
    return NULL;
}

int net_socket_bind(socket_t* socket, uint16_t port) {
    if (!socket || !socket->is_open) return -1;
    socket->local_port = port;
    return 0;
}

int net_socket_connect(socket_t* socket, ip_addr_t* ip, uint16_t port) {
    if (!socket || !socket->is_open || !ip) return -1;
    memcpy(&socket->remote_ip, ip, sizeof(ip_addr_t));
    socket->remote_port = port;

    if (socket->protocol != PROTO_TCP) {
        return 0;   /* UDP/other: connect() just remembers the peer, as before */
    }
    if (!nic_present) return -1;

    if (socket->local_port == 0) {
        socket->local_port = (uint16_t)(49152 + (socket->id % 16384));
    }

    /* Initial sequence number: timer ticks mixed with the socket's own
     * slot index. Enough entropy to avoid colliding with a socket this
     * same kernel opened moments ago; not a defense against off-path ISN
     * guessing - no crypto/hardware RNG is available here, and this
     * stack's threat model is "talk to a cooperative test server", not
     * "resist spoofing", consistent with the rest of this minimal client
     * TCP scope (see tcp_state_t's doc comment). */
    socket->snd_nxt = (uint32_t)timer_get_ticks() * 65537u + socket->id;
    socket->rcv_nxt = 0;
    socket->tcp_state = TCP_SYN_SENT;

    /* net_socket_connect() is only ever called from ordinary process/shell
     * context (never the RTL8139 IRQ handler), so blocking ARP resolution
     * here is safe - see send_ip_packet()'s doc comment. */
    if (send_tcp_segment(socket, TCP_FLAG_SYN, 0, 0, 1) != 0) {
        socket->tcp_state = TCP_CLOSED;
        return -1;
    }

    uint64_t deadline = timer_get_ticks() + 100;   /* ~1s @ 100 Hz, matches net_ping()/net_arp_resolve() */
    while (timer_get_ticks() < deadline) {
        if (socket->tcp_state == TCP_ESTABLISHED) return 0;
        if (socket->tcp_state == TCP_CLOSED) return -1;   /* RST received */
        __asm__ __volatile__("pause");
    }

    socket->tcp_state = TCP_CLOSED;
    return -1;   /* timeout - no SYN-ACK within ~1s */
}

int net_socket_send(socket_t* socket, const void* data, size_t size) {
    if (!socket || !socket->is_open || !data) return -1;

    if (socket->protocol == PROTO_TCP) {
        if (socket->tcp_state != TCP_ESTABLISHED) return -1;
        if (size > TCP_MAX_PAYLOAD) size = TCP_MAX_PAYLOAD;   /* one segment, no fragmentation */
        /* Only ever called from process/shell context - see
         * net_socket_connect()'s identical comment on allow_arp_wait. */
        if (send_tcp_segment(socket, TCP_FLAG_PSH | TCP_FLAG_ACK,
                             (const uint8_t *)data, (uint16_t)size, 1) != 0) {
            return -1;
        }
        return (int)size;
    }

    if (socket->protocol != PROTO_UDP) {
        /* Other protocol values behave as inert placeholders. */
        return -1;
    }
    if (!nic_present) return -1;
    if (size > SOCK_RX_MAX_LEN) return -1;   /* keep within one MTU-ish datagram */

    if (socket->local_port == 0) {
        /* Auto-assign an ephemeral port, like a real bind()-on-first-use. */
        socket->local_port = (uint16_t)(49152 + (socket->id % 16384));
    }

    uint8_t buf[sizeof(udp_header_t) + SOCK_RX_MAX_LEN];
    udp_header_t *udp = (udp_header_t *)buf;
    udp->src_port = net_htons(socket->local_port);
    udp->dst_port = net_htons(socket->remote_port);
    udp->length = net_htons((uint16_t)(sizeof(udp_header_t) + size));
    udp->checksum = 0;   /* Optional for IPv4 UDP (RFC 768); left unset */
    memcpy(buf + sizeof(udp_header_t), data, size);

    /* net_socket_send() is only ever called from ordinary process/shell
     * context (never from the RTL8139 IRQ handler), so blocking ARP
     * resolution here is safe - see send_ip_packet()'s doc comment. */
    int sent = send_ip_packet(&socket->remote_ip, PROTO_UDP, buf, (uint16_t)(sizeof(udp_header_t) + size), 1);
    return sent < 0 ? -1 : (int)size;
}

int net_socket_recv(socket_t* socket, void* buffer, size_t size) {
    if (!socket || !socket->is_open || !buffer) return -1;
    if ((socket->protocol != PROTO_UDP && socket->protocol != PROTO_TCP) || socket->rx_count == 0) {
        return 0;   /* Nothing queued (non-blocking, matches the old stub's "no data" return) */
    }

    uint8_t slot = socket->rx_head;
    uint16_t len = socket->rx_len[slot];
    if (len > size) len = (uint16_t)size;

    memcpy(buffer, socket->rx_queue[slot], len);

    /* Reply-to-last-sender convenience, like a connected UDP socket
     * that just learned who it's talking to. */
    socket->remote_ip = socket->rx_src_ip[slot];
    socket->remote_port = socket->rx_src_port[slot];

    socket->rx_head = (uint8_t)((socket->rx_head + 1) % SOCK_RX_QUEUE_LEN);
    socket->rx_count--;

    return (int)len;
}

void net_socket_close(socket_t* socket) {
    if (!socket) return;

    if (socket->protocol == PROTO_TCP && socket->is_open) {
        if (socket->tcp_state == TCP_ESTABLISHED) {
            send_tcp_segment(socket, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0, 1);
            socket->tcp_state = TCP_FIN_WAIT1;
            /* Best-effort: give the peer's FIN/ACK a brief window to land
             * before the socket slot is released and handle_tcp() stops
             * recognizing it. Not required for correctness of the close
             * itself (no TIME_WAIT bookkeeping in this minimal scope -
             * see tcp_state_t's doc comment) - just a courtesy so a
             * prompt reconnect on the same local port doesn't immediately
             * race a stray in-flight FIN/ACK from this connection. */
            uint64_t deadline = timer_get_ticks() + 50;   /* ~500 ms */
            while (timer_get_ticks() < deadline && socket->tcp_state != TCP_CLOSED) {
                __asm__ __volatile__("pause");
            }
        } else if (socket->tcp_state == TCP_CLOSE_WAIT) {
            send_tcp_segment(socket, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0, 1);
        }
        socket->tcp_state = TCP_CLOSED;
    }

    socket->is_open = 0;
}

uint16_t net_checksum(const void* data, size_t length) {
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;

    while (length > 1) {
        sum += *ptr++;
        length -= 2;
    }

    if (length > 0) {
        sum += *(uint8_t*)ptr;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}
