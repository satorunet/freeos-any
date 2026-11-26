#include "net.h"

typedef struct {
    u32 state, hwSize, headerSize, maxPacket, nvSize, nvAccess, filterMask, filter, maxMcast, mcastCount;
    u8 mcast[16][32], mac[32], broadcast[32], permanent[32];
    u8 ifType, macChangeable, multiTx, mediaSupported, mediaPresent;
} SnpMode;
typedef struct Snp {
    u64 rev;
    Status (*Start)(struct Snp *);
    void *Stop;
    Status (*Initialize)(struct Snp *, u64, u64);
    void *Reset, *Shutdown;
    Status (*ReceiveFilters)(struct Snp *, u32, u32, u8, u64, void *);
    void *StationAddress, *Statistics, *MCastIpToMac, *NvData;
    Status (*GetStatus)(struct Snp *, u32 *, void **);
    Status (*Transmit)(struct Snp *, u64, u64, void *, void *, void *, u16 *);
    Status (*Receive)(struct Snp *, u64 *, u64 *, void *, void *, void *, u16 *);
    Event wait;
    SnpMode *mode;
} Snp;

static Snp *snp;
static u8 mac[6], txbuf[1600], rxbuf[2048];
static u32 myip, netmask, gateway, dns;
static const c16 *error = u"";

void net_set_error(const c16 *m) { error = m; }
const c16 *net_error(void) { return error; }
int net_ready(void) { return myip != 0; }
u32 net_ip(void) { return myip; }
void net_ipstr(u32 ip, c16 *out, int cap) {
    out[0] = 0;
    for (int i = 3; i >= 0; i--) { snum(out, (ip >> (i * 8)) & 255, 0, cap); if (i) scat(out, u".", cap); }
}

/* ---- 時計 ---- */
static u64 counter(void) {
#if defined(__aarch64__)
    u64 v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v;
#else
    u32 a, d; __asm__ volatile("rdtsc" : "=a"(a), "=d"(d)); return (u64)d << 32 | a;
#endif
}
u64 net_ms(void) {
    static u64 per_ms, base;
    if (!per_ms) {
        base = counter(); BS->Stall(20000);
        per_ms = (counter() - base) / 20;
        if (!per_ms) per_ms = 1;
    }
    return (counter() - base) / per_ms;
}

/* ---- 暗号用の乱数: ファームウェアの RNG か CPU の命令だけを使う ---- */
int entropy(u8 *out, int n) {
    static Guid g = {0x3152bca5, 0xeade, 0x433d, {0x86,0x2e,0xc0,0x1c,0xdc,0x29,0x1f,0x44}};
    struct Rng { void *info; Status (*GetRNG)(struct Rng *, void *, u64, u8 *); } *rng;
    if (!BS->LocateProtocol(&g, 0, (void **)&rng) && !rng->GetRNG(rng, 0, n, out)) return 1;
#if defined(__aarch64__)
    u64 isar; __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar));
    if (!(isar >> 60)) return 0;
#else
    u32 a, b, c, d; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1u << 30))) return 0;
#endif
    for (int i = 0; i < n;) {
        u64 v = 0; int ok = 0;
        for (int t = 0; t < 100 && !ok; t++) {
#if defined(__aarch64__)
            u64 f; __asm__ volatile("mrs %0, s3_3_c2_c4_0\n cset %1, ne" : "=r"(v), "=r"(f)); ok = (int)f;
#else
            u8 f; __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(f)); ok = f;
#endif
        }
        if (!ok) return 0;
        for (int k = 0; k < 8 && i < n; k++) out[i++] = (u8)(v >> (k * 8));
    }
    return 1;
}
static u32 random32(void) { u32 v; if (!entropy((u8 *)&v, 4)) v = (u32)counter() * 2654435761u; return v; }

/* ---- バイト列 ---- */
static u16 get16(const u8 *p) { return p[0] << 8 | p[1]; }
static u32 get32(const u8 *p) { return (u32)get16(p) << 16 | get16(p + 2); }
static void put16(u8 *p, u32 v) { p[0] = v >> 8; p[1] = v; }
static void put32(u8 *p, u32 v) { put16(p, v >> 16); put16(p + 2, v); }
static u32 sum(const u8 *p, int n, u32 s) {
    for (; n > 1; p += 2, n -= 2) s += get16(p);
    if (n) s += p[0] << 8;
    return s;
}
static u16 fold(u32 s) { while (s >> 16) s = (s & 0xFFFF) + (s >> 16); return ~s; }

/* ---- Ethernet / ARP / IPv4 ---- */
static struct { u32 ip; u8 mac[6]; } arp[16];
static int arpNext;
static const u8 bcast[6] = {255, 255, 255, 255, 255, 255};

static void transmit(const u8 *dst, u16 type, int payload) { /* 中身は txbuf+14 に用意済み */
    int len = 14 + payload;
    mcopy(txbuf, dst, 6); mcopy(txbuf + 6, mac, 6); put16(txbuf + 12, type);
    while (len < 60) txbuf[len++] = 0;
    if (snp->Transmit(snp, 0, len, txbuf, 0, 0, 0)) return;
    for (int i = 0; i < 1000; i++) { /* バッファが返るまで待つ */
        u32 irq; void *done = 0;
        if (snp->GetStatus(snp, &irq, &done) || done) break;
        BS->Stall(10);
    }
}
static void arp_learn(u32 ip, const u8 *m) {
    int i = 0;
    while (i < 16 && arp[i].ip != ip) i++;
    if (i == 16) i = arpNext++ % 16;
    arp[i].ip = ip; mcopy(arp[i].mac, m, 6);
}
static const u8 *arp_find(u32 ip) { for (int i = 0; i < 16; i++) if (arp[i].ip == ip) return arp[i].mac; return 0; }
static void arp_send(int op, const u8 *dmac, u32 dip) {
    u8 *p = txbuf + 14;
    put16(p, 1); put16(p + 2, 0x800); p[4] = 6; p[5] = 4; put16(p + 6, op);
    mcopy(p + 8, mac, 6); put32(p + 14, myip);
    if (op == 2) mcopy(p + 18, dmac, 6); else memset(p + 18, 0, 6);
    put32(p + 24, dip);
    transmit(op == 2 ? dmac : bcast, 0x806, 28);
}
static u32 next_hop(u32 ip) { return (ip & netmask) == (myip & netmask) || !gateway ? ip : gateway; }
/* 送り先の MAC が未知なら問い合わせだけして捨てる (再送に任せる) */
static void ip_send(u32 dst, int proto, int len) { /* 中身は txbuf+34 に用意済み */
    static u16 ident;
    const u8 *m = dst == 0xFFFFFFFF ? bcast : arp_find(next_hop(dst));
    if (!m) { arp_send(1, 0, next_hop(dst)); return; }
    u8 *p = txbuf + 14;
    p[0] = 0x45; p[1] = 0; put16(p + 2, 20 + len); put16(p + 4, ident++); put16(p + 6, 0x4000);
    p[8] = 64; p[9] = proto; put16(p + 10, 0); put32(p + 12, myip); put32(p + 16, dst);
    put16(p + 10, fold(sum(p, 20, 0)));
    transmit(m, 0x800, 20 + len);
}
static void udp_send(u32 dst, u16 sport, u16 dport, const u8 *data, int len) {
    u8 *u = txbuf + 34;
    put16(u, sport); put16(u + 2, dport); put16(u + 4, 8 + len); put16(u + 6, 0);
    mcopy(u + 8, data, len);
    ip_send(dst, 17, 8 + len);
}

/* ---- DHCP / DNS の受信 ---- */
static u32 dhcpXid, offerIp, offerServer;
static int dhcpGot; /* 2=OFFER 5=ACK 6=NAK */
static u32 dnsAnswer; static u16 dnsId, dnsPort; static int dnsDone;

static void dhcp_input(const u8 *p, int n) {
    if (n < 244 || p[0] != 2 || get32(p + 4) != dhcpXid) return;
    for (int i = 0; i < 6; i++) if (p[28 + i] != mac[i]) return;
    int type = 0; u32 mask = 0, gw = 0, ns = 0, server = 0;
    for (int i = 240; i + 1 < n && p[i] != 255;) {
        if (!p[i]) { i++; continue; }
        int o = p[i], l = p[i + 1]; const u8 *v = p + i + 2;
        if (i + 2 + l > n) break;
        if (o == 53 && l) type = v[0];
        if (l >= 4) { if (o == 1) mask = get32(v); if (o == 3) gw = get32(v); if (o == 6) ns = get32(v); if (o == 54) server = get32(v); }
        i += 2 + l;
    }
    if (type == 2 && dhcpGot == 0) { offerIp = get32(p + 16); offerServer = server; dhcpGot = 2; }
    else if (type == 5 && dhcpGot == 2) { myip = get32(p + 16); netmask = mask ? mask : 0xFFFFFF00; gateway = gw; dns = ns; dhcpGot = 5; }
    else if (type == 6) dhcpGot = 6;
}
static int dns_skip(const u8 *p, int n, int i) {
    while (i < n) { if (!p[i]) return i + 1; if ((p[i] & 0xC0) == 0xC0) return i + 2; i += p[i] + 1; }
    return n;
}
static void dns_input(const u8 *p, int n) {
    if (n < 12 || get16(p) != dnsId || !(p[2] & 0x80)) return;
    int qd = get16(p + 4), an = get16(p + 6), i = 12;
    while (qd-- && i < n) i = dns_skip(p, n, i) + 4;
    while (an-- && i < n) {
        i = dns_skip(p, n, i);
        if (i + 10 > n) break;
        int type = get16(p + i), len = get16(p + i + 8);
        if (type == 1 && len == 4 && i + 14 <= n) { dnsAnswer = get32(p + i + 10); break; }
        i += 10 + len;
    }
    dnsDone = 1;
}

/* ---- TCP ---- */
#define NSOCK 8
#define BUFSZ (32 * 1024)
#define MSS 1460
enum { T_FREE, T_SYN_SENT, T_SYN_RCVD, T_OPEN, T_DEAD };
enum { FIN = 1, SYN = 2, RST = 4, PSH = 8, ACK = 16 };
typedef struct {
    int state, accepted, peerFin, closing, finSent, finAcked, tries, passive;
    u32 ip, una, nxt, max, rcv, wnd;
    u16 lport, rport;
    int rxLen, txLen;          /* tx は una から先の未確認データ */
    u64 timer, closeAt;
    u8 *rx, *tx;
} Sock;
static Sock sock[NSOCK];
static u16 listenPort;

static void tcp_output(u32 dst, u16 sport, u16 dport, u32 seq, u32 ack, int flags, int wnd, const u8 *data, int len) {
    u8 *t = txbuf + 34;
    int hl = flags & SYN ? 24 : 20;
    if (len) mcopy(t + hl, data, len);
    put16(t, sport); put16(t + 2, dport); put32(t + 4, seq); put32(t + 8, ack);
    t[12] = (hl / 4) << 4; t[13] = flags; put16(t + 14, wnd); put16(t + 16, 0); put16(t + 18, 0);
    if (flags & SYN) { t[20] = 2; t[21] = 4; put16(t + 22, MSS); }
    u32 s = (myip >> 16) + (myip & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + 6 + hl + len;
    put16(t + 16, fold(sum(t, hl + len, s)));
    ip_send(dst, 6, hl + len);
}
static void segment(Sock *k, u32 seq, int flags, const u8 *d, int len) {
    int wnd = BUFSZ - k->rxLen;
    if (wnd > 16 * MSS) wnd = 16 * MSS; /* 受信バッファあふれを防ぐため控えめに */
    tcp_output(k->ip, k->lport, k->rport, seq, k->rcv, flags, wnd, d, len);
}
static void ack_now(Sock *k) { segment(k, k->nxt, ACK, 0, 0); }
static void push(Sock *k) { /* 送れる分を送る */
    if (k->state != T_OPEN) return;
    for (;;) {
        int off = (int)(k->nxt - k->una);
        int n = k->txLen - off;
        if (n <= 0) break;
        if (n > MSS) n = MSS;
        int room = (int)(k->wnd ? k->wnd : 1) - off;
        if (room <= 0) break;
        if (n > room) n = room;
        if (off == 0) k->timer = net_ms();
        segment(k, k->nxt, ACK | PSH, k->tx + off, n);
        k->nxt += n;
        if ((i32)(k->nxt - k->max) > 0) k->max = k->nxt;
    }
    if (k->closing && !k->finAcked && (int)(k->nxt - k->una) == k->txLen) {
        if (k->txLen == 0) k->timer = net_ms();
        segment(k, k->nxt, ACK | FIN, 0, 0);
        k->finSent = 1; k->nxt++;
        if ((i32)(k->nxt - k->max) > 0) k->max = k->nxt;
    }
}
static int sock_new(void) {
    for (int i = 0; i < NSOCK; i++) {
        Sock *k = &sock[i];
        if (k->state != T_FREE) continue;
        u8 *rx = k->rx, *tx = k->tx;
        if (!rx && (BS->AllocatePool(2, BUFSZ, (void **)&rx) || BS->AllocatePool(2, BUFSZ, (void **)&tx))) return -1;
        memset(k, 0, sizeof *k);
        k->rx = rx; k->tx = tx;
        return i;
    }
    return -1;
}
static void tcp_input(u32 src, const u8 *t, int n) {
    if (n < 20) return;
    u32 s = (src >> 16) + (src & 0xFFFF) + (myip >> 16) + (myip & 0xFFFF) + 6 + n;
    if (fold(sum(t, n, s))) return; /* 壊れたパケット */
    u16 sport = get16(t), dport = get16(t + 2);
    u32 seq = get32(t + 4), ack = get32(t + 8);
    int hl = (t[12] >> 4) * 4, flags = t[13];
    if (hl < 20 || hl > n) return;
    const u8 *d = t + hl; int len = n - hl;
    Sock *k = 0;
    for (int i = 0; i < NSOCK; i++)
        if (sock[i].state != T_FREE && sock[i].state != T_DEAD && sock[i].ip == src && sock[i].rport == sport && sock[i].lport == dport) k = &sock[i];
    if (!k) {
        if ((flags & (SYN | ACK | RST)) == SYN && listenPort && dport == listenPort) {
            int i = sock_new();
            if (i < 0) return;
            k = &sock[i];
            k->state = T_SYN_RCVD; k->passive = 1; k->ip = src; k->rport = sport; k->lport = dport;
            k->rcv = seq + 1; k->una = random32(); k->nxt = k->max = k->una + 1; k->wnd = get16(t + 14); k->timer = net_ms();
            segment(k, k->una, SYN | ACK, 0, 0);
        } else if (!(flags & RST)) tcp_output(src, dport, sport, flags & ACK ? ack : 0, seq + len + !!(flags & SYN), RST | ACK, 0, 0, 0);
        return;
    }
    if (flags & RST) {
        if (k->state == T_SYN_SENT ? ack == k->nxt : seq - k->rcv < BUFSZ) { if (k->closing || (k->passive && !k->accepted)) k->state = T_FREE; else k->state = T_DEAD; }
        return;
    }
    if (k->state == T_SYN_SENT) {
        if ((flags & (SYN | ACK)) != (SYN | ACK) || ack != k->nxt) return;
        k->rcv = seq + 1; k->una = ack; k->wnd = get16(t + 14); k->state = T_OPEN; k->tries = 0;
        ack_now(k);
        return;
    }
    if (!(flags & ACK)) return;
    if (k->state == T_SYN_RCVD) {
        if (ack != k->nxt) return;
        k->state = T_OPEN; k->una = ack; k->tries = 0;
    }
    u32 adv = ack - k->una;
    if (adv && adv <= k->max - k->una) {
        int bytes = adv > (u32)k->txLen ? k->txLen : (int)adv;
        if ((int)adv > bytes && k->finSent) k->finAcked = 1;
        mcopy(k->tx, k->tx + bytes, k->txLen - bytes);
        k->txLen -= bytes; k->una = ack; k->tries = 0; k->timer = net_ms();
        if ((i32)(k->nxt - k->una) < 0) k->nxt = k->una;
    }
    k->wnd = get16(t + 14);
    int need = len > 0 || (flags & (FIN | SYN));
    u32 skip = k->rcv - seq; /* 受信済みの重なりは捨てる */
    if (len && skip < (u32)len) { d += skip; len -= skip; seq = k->rcv; }
    if (seq == k->rcv && !k->peerFin) {
        int take = len > BUFSZ - k->rxLen ? BUFSZ - k->rxLen : len;
        if (k->closing) take = len; /* もう読まないので捨てる */
        else { mcopy(k->rx + k->rxLen, d, take); k->rxLen += take; }
        k->rcv += take;
        if ((flags & FIN) && take == len) { k->rcv++; k->peerFin = 1; }
    }
    if (need) ack_now(k);
    if (k->closing && k->finAcked && k->peerFin) k->state = T_FREE; else push(k);
}
static void tcp_timers(void) {
    u64 now = net_ms();
    for (int i = 0; i < NSOCK; i++) {
        Sock *k = &sock[i];
        if (k->state == T_FREE) continue;
        if (k->closing && now > k->closeAt) { k->state = T_FREE; continue; }
        if (k->state == T_DEAD) continue;
        int waiting = k->state != T_OPEN || k->txLen || (k->finSent && !k->finAcked);
        if (!waiting || now - k->timer < (500u << (k->tries > 3 ? 3 : k->tries))) continue;
        if (++k->tries > 8) { k->state = k->closing || (k->passive && !k->accepted) ? T_FREE : T_DEAD; continue; }
        k->timer = now;
        if (k->state == T_SYN_SENT) segment(k, k->una, SYN, 0, 0);
        else if (k->state == T_SYN_RCVD) segment(k, k->una, SYN | ACK, 0, 0);
        else { k->nxt = k->una; k->finSent = 0; push(k); }
    }
}

int tcp_listen(u16 port) { listenPort = port; return 1; }
void tcp_unlisten(void) {
    listenPort = 0;
    for (int i = 0; i < NSOCK; i++) if (sock[i].state != T_FREE && sock[i].passive) tcp_abort(i);
}
int tcp_accept(void) {
    for (int i = 0; i < NSOCK; i++) if (sock[i].state == T_OPEN && sock[i].passive && !sock[i].accepted) { sock[i].accepted = 1; return i; }
    return -1;
}
void tcp_abort(int s) {
    Sock *k = &sock[s];
    if (k->state == T_OPEN || k->state == T_SYN_RCVD) segment(k, k->nxt, RST | ACK, 0, 0);
    k->state = T_FREE;
}
void tcp_close(int s) {
    Sock *k = &sock[s];
    if (k->state != T_OPEN) { tcp_abort(s); return; }
    k->closing = 1; k->closeAt = net_ms() + 10000; k->rxLen = 0;
    push(k);
}
int tcp_send(int s, const void *buf, int len) {
    Sock *k = &sock[s];
    if (k->state != T_OPEN || k->closing) return -1;
    if (len > BUFSZ - k->txLen) len = BUFSZ - k->txLen;
    mcopy(k->tx + k->txLen, buf, len); k->txLen += len;
    push(k);
    return len;
}
int tcp_sent_all(int s) { return sock[s].state == T_OPEN && !sock[s].txLen; }
int tcp_recv(int s, void *buf, int cap) {
    Sock *k = &sock[s];
    if (k->rxLen) {
        int n = cap < k->rxLen ? cap : k->rxLen, was = BUFSZ - k->rxLen;
        mcopy(buf, k->rx, n);
        mcopy(k->rx, k->rx + n, k->rxLen - n);
        k->rxLen -= n;
        if (was < 2 * MSS && k->state == T_OPEN) ack_now(k); /* 窓が開いたことを知らせる */
        return n;
    }
    if (k->peerFin) return -1;
    return k->state == T_OPEN ? 0 : -2;
}

/* ---- 受信 ---- */
static void input(const u8 *f, int n) {
    if (n < 34) return;
    u16 type = get16(f + 12);
    const u8 *p = f + 14; n -= 14;
    if (type == 0x806) {
        if (n < 28 || get16(p + 6) < 1) return;
        u32 sip = get32(p + 14), tip = get32(p + 24);
        if (myip && tip == myip) { arp_learn(sip, p + 8); if (get16(p + 6) == 1) arp_send(2, p + 8, sip); }
        else if (arp_find(sip)) arp_learn(sip, p + 8);
        return;
    }
    if (type != 0x800 || (p[0] >> 4) != 4) return;
    int ihl = (p[0] & 15) * 4, total = get16(p + 2);
    if (ihl < 20 || total < ihl || total > n || fold(sum(p, ihl, 0)) || (get16(p + 6) & 0x3FFF)) return;
    u32 src = get32(p + 12), dst = get32(p + 16);
    const u8 *d = p + ihl; int len = total - ihl;
    if (myip && dst == myip && (src & netmask) == (myip & netmask)) arp_learn(src, f + 6);
    if (p[9] == 17 && len >= 8) {
        u16 sport = get16(d), dport = get16(d + 2);
        if (dport == 68 && sport == 67) dhcp_input(d + 8, len - 8);
        else if (dst == myip && src == dns && sport == 53 && dport == dnsPort) dns_input(d + 8, len - 8);
    } else if (dst == myip && myip) {
        if (p[9] == 6) tcp_input(src, d, len);
        else if (p[9] == 1 && len >= 8 && len <= 1480 && d[0] == 8) { /* ping に応答 */
            u8 *o = txbuf + 34;
            mcopy(o, d, len); o[0] = 0; put16(o + 2, 0); put16(o + 2, fold(sum(o, len, 0)));
            ip_send(src, 1, len);
        }
    }
}
void net_poll(void) {
    if (!snp) return;
    for (int i = 0; i < 64; i++) {
        u64 hs = 0, n = sizeof rxbuf; u16 proto;
        if (snp->Receive(snp, &hs, &n, rxbuf, 0, 0, &proto)) break;
        input(rxbuf, (int)n);
    }
    tcp_timers();
}
int net_wait(void) {
    Key k;
    if (!ST->conIn->ReadKeyStroke(ST->conIn, &k) && k.scan == 0x17) { net_set_error(u"中止しました"); return 0; }
    net_poll();
    BS->Stall(200);
    return 1;
}

/* ---- 初期化 ---- */
static int open_adapter(Handle image) {
    static Guid g = {0xA19832B9, 0xAC25, 0x11D3, {0x9A,0x2D,0x00,0x90,0x27,0x3F,0xC1,0x4D}};
    u64 n; Handle *h;
    if (BS->LocateHandleBuffer(2, &g, 0, &n, &h)) return 0;
    Snp *found = 0;
    for (u64 i = 0; i < n && !found; i++) {
        Snp *s;
        /* ファームウェアの通信機能と取り合いにならないよう専有する */
        if (BS->OpenProtocol(h[i], &g, (void **)&s, image, 0, 0x20) && BS->HandleProtocol(h[i], &g, (void **)&s)) continue;
        if (s->mode->state == 0 && s->Start(s)) continue;
        if (s->mode->state == 1 && s->Initialize(s, 0, 0)) continue;
        if (s->mode->state != 2 || s->mode->hwSize != 6 || s->mode->headerSize != 14) continue;
        s->ReceiveFilters(s, 1 | 4, 0, 0, 0, 0);
        found = s;
    }
    BS->FreePool(h);
    if (!found) return 0;
    snp = found;
    mcopy(mac, snp->mode->mac, 6);
    return 1;
}
static Handle self;
void net_init(Handle image) { self = image; }
static void dhcp_send(int type) {
    static u8 m[300];
    memset(m, 0, sizeof m);
    m[0] = 1; m[1] = 1; m[2] = 6; put32(m + 4, dhcpXid); put16(m + 10, 0x8000);
    mcopy(m + 28, mac, 6);
    put32(m + 236, 0x63825363);
    int i = 240;
    m[i++] = 53; m[i++] = 1; m[i++] = type;
    if (type == 3) { m[i++] = 50; m[i++] = 4; put32(m + i, offerIp); i += 4; m[i++] = 54; m[i++] = 4; put32(m + i, offerServer); i += 4; }
    m[i++] = 55; m[i++] = 3; m[i++] = 1; m[i++] = 3; m[i++] = 6;
    m[i++] = 255;
    udp_send(0xFFFFFFFF, 68, 67, m, sizeof m);
}
int net_up(void) {
    if (myip) return 1;
    if (!snp && !open_adapter(self)) { net_set_error(u"使える有線LANがありません"); return 0; }
    for (int attempt = 0; attempt < 4; attempt++) {
        dhcpXid = random32(); dhcpGot = 0;
        dhcp_send(1);
        u64 end = net_ms() + 2500;
        int asked = 0;
        while (net_ms() < end) {
            if (!net_wait()) return 0;
            if (dhcpGot == 2 && !asked) { dhcp_send(3); asked = 1; }
            if (dhcpGot == 5) { arp_send(1, 0, next_hop(dns ? dns : gateway)); return 1; }
            if (dhcpGot == 6) break;
        }
    }
    net_set_error(u"IPアドレスを取得できません (LANケーブルを確認)");
    return 0;
}
static int arp_wait(u32 ip) {
    u32 hop = next_hop(ip);
    for (int t = 0; t < 4 && !arp_find(hop); t++) {
        arp_send(1, 0, hop);
        u64 end = net_ms() + 600;
        while (!arp_find(hop) && net_ms() < end) if (!net_wait()) return 0;
    }
    if (!arp_find(hop)) { net_set_error(u"相手に届きません"); return 0; }
    return 1;
}
int net_resolve(const char *host, u32 *ip) {
    u32 v = 0, part = 0; int dots = 0, digits = 0, numeric = 1;
    for (const char *p = host;; p++) {
        if (*p >= '0' && *p <= '9' && part < 256) part = part * 10 + *p - '0', digits++;
        else if ((*p == '.' || !*p) && digits && part < 256) { v = v << 8 | part; part = digits = 0; if (!*p) break; dots++; }
        else { numeric = 0; break; }
    }
    if (numeric && dots == 3) { *ip = v; return 1; }
    static u8 q[300];
    memset(q, 0, 12);
    dnsId = random32(); dnsPort = 49152 + random32() % 16000;
    put16(q, dnsId); q[2] = 1; q[5] = 1;
    int i = 12;
    while (*host) {
        int l = 0;
        while (host[l] && host[l] != '.') l++;
        if (!l || l > 63 || i + l + 6 > (int)sizeof q) { net_set_error(u"アドレスが正しくありません"); return 0; }
        q[i++] = l; mcopy(q + i, host, l); i += l; host += l;
        if (*host == '.') host++;
    }
    q[i++] = 0; put16(q + i, 1); put16(q + i + 2, 1); i += 4;
    if (!dns || !arp_wait(dns)) { net_set_error(u"DNSサーバーに接続できません"); return 0; }
    dnsDone = 0; dnsAnswer = 0;
    for (int t = 0; t < 3 && !dnsDone; t++) {
        udp_send(dns, dnsPort, 53, q, i);
        u64 end = net_ms() + 2000;
        while (!dnsDone && net_ms() < end) if (!net_wait()) return 0;
    }
    if (!dnsAnswer) { net_set_error(u"そのサイト名が見つかりません"); return 0; }
    *ip = dnsAnswer;
    return 1;
}
int tcp_connect(u32 ip, u16 port) {
    if (!arp_wait(ip)) return -1;
    int s = sock_new();
    if (s < 0) { net_set_error(u"接続が多すぎます"); return -1; }
    Sock *k = &sock[s];
    k->state = T_SYN_SENT; k->ip = ip; k->rport = port; k->lport = 49152 + random32() % 16000;
    k->una = random32(); k->nxt = k->max = k->una + 1; k->timer = net_ms();
    segment(k, k->una, SYN, 0, 0);
    u64 end = net_ms() + 12000;
    while (k->state == T_SYN_SENT && net_ms() < end) if (!net_wait()) { tcp_abort(s); return -1; }
    if (k->state != T_OPEN) { tcp_abort(s); net_set_error(u"接続できません"); return -1; }
    return s;
}
