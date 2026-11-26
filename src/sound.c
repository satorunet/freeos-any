/* 起動音: 仮想マシン用の音声装置 (virtio-sound) ドライバと、起動チャイムの合成。
 * virtio.c (タブレット) と同じく PCI を直接操作する (virtio 1.0)。
 * 実機や音声装置のない仮想マシンでは何もしない。 */
#include "os.h"

typedef struct PciIo PciIo;
typedef Status (*PciMem)(PciIo *, u32 width, u8 bar, u64 off, u64 count, void *buf);
typedef Status (*PciCfg)(PciIo *, u32 width, u32 off, u64 count, void *buf);
struct PciIo {
    void *PollMem, *PollIo;
    PciMem MemRead, MemWrite;
    void *IoRead, *IoWrite;
    PciCfg CfgRead, CfgWrite;
    void *CopyMem;
    Status (*Map)(PciIo *, u32, void *, u64 *, u64 *, void **);
    void *Unmap;
    Status (*AllocateBuffer)(PciIo *, u32, u32, u64, void **, u64);
    void *FreeBuffer, *Flush, *GetLocation;
    Status (*Attributes)(PciIo *, u32, u64, u64 *);
};
enum { W8, W16, W32 };
typedef struct { u8 bar; u32 off; int found; } Region;

#define QMAX 64
typedef struct { u64 addr; u32 len; u16 flags, next; } Desc;
typedef struct { u16 flags, idx, ring[QMAX]; } Avail;
typedef struct { u16 flags, idx; struct { u32 id, len; } ring[QMAX]; } Used;
typedef struct {
    Desc *d; volatile Avail *avail; volatile Used *used;
    u16 size, seen, nfree, free[QMAX], notify;
} Queue;

/* 音の形式: 48kHz / 16ビット / ステレオ */
#define RATE 48000
#define SECONDS 3
#define FRAMES (RATE * SECONDS)
#define PERIOD (RATE / 5) /* 0.2秒ごとに装置へ渡す */
#define NPERIOD (FRAMES / PERIOD)

i16 *hda_init(u32 frames); void hda_start(void); void hda_stop(void); u32 hda_pos(void);
u64 net_ms(void);

static struct {
    int hda; u64 hdaEnd; u32 lastPos; /* virtio-sound がなければ HDA を使う */
    PciIo *io;
    Region common, notify, device;
    u32 notifyMul;
    Queue ctl, tx;
    u8 *cbuf; u64 cdev;           /* 制御要求とその返事 (1ページ) */
    i16 *pcm; u64 pdev;           /* 音のデータ */
    u32 stream;
    int ready, playing, sent, done;
} s;

static u32 rd(Region *r, u32 off, int w) { u32 v = 0; s.io->MemRead(s.io, w, r->bar, r->off + off, 1, &v); return v; }
static void wr(Region *r, u32 off, int w, u32 v) { s.io->MemWrite(s.io, w, r->bar, r->off + off, 1, &v); }
static u32 cfg(u32 off, int w) { u32 v = 0; s.io->CfgRead(s.io, w, off, 1, &v); return v; }

static void find_regions(void) {
    if (!(cfg(0x06, W16) & 0x10)) return;
    u32 p = cfg(0x34, W8) & 0xFC;
    for (int n = 0; p && n < 48; n++, p = cfg(p + 1, W8) & 0xFC) {
        if (cfg(p, W8) != 0x09) continue;
        u32 type = cfg(p + 3, W8);
        Region *r = type == 1 ? &s.common : type == 2 ? &s.notify : type == 4 ? &s.device : 0;
        if (!r || r->found) continue;
        r->bar = cfg(p + 4, W8); r->off = cfg(p + 8, W32); r->found = 1;
        if (type == 2) s.notifyMul = cfg(p + 16, W32);
    }
}

/* DMA 用のメモリ: 装置から見えるアドレスを dev に返す */
static void *dma(u64 pages, u64 *dev) {
    void *mem, *map; u64 bytes = pages * 4096;
    if (s.io->AllocateBuffer(s.io, 0, 4, pages, &mem, 0)) return 0;
    if (s.io->Map(s.io, 2, mem, &bytes, dev, &map) || bytes < pages * 4096) return 0;
    memset(mem, 0, pages * 4096);
    return mem;
}

static int queue_setup(Queue *q, u16 index) {
    Region *c = &s.common;
    wr(c, 22, W16, index);
    u32 size = rd(c, 24, W16);
    if (!size) return 0;
    if (size > QMAX) size = QMAX;
    if (size & (size - 1)) return 0;
    u64 dev; u8 *mem = dma(1, &dev);
    if (!mem) return 0;
    /* 1ページの中に: 記述子 0, avail 1024, used 2048 */
    q->d = (Desc *)mem; q->avail = (void *)(mem + 1024); q->used = (void *)(mem + 2048);
    q->size = size; q->seen = 0; q->nfree = size;
    for (u16 i = 0; i < size; i++) q->free[i] = size - 1 - i;
    wr(c, 24, W16, size);
    wr(c, 32, W32, (u32)dev); wr(c, 36, W32, (u32)(dev >> 32));
    wr(c, 40, W32, (u32)(dev + 1024)); wr(c, 44, W32, (u32)((dev + 1024) >> 32));
    wr(c, 48, W32, (u32)(dev + 2048)); wr(c, 52, W32, (u32)((dev + 2048) >> 32));
    q->notify = rd(c, 30, W16);
    wr(c, 28, W16, 1);
    return 1;
}
static void kick(Queue *q, u16 index) {
    s.io->MemWrite(s.io, W16, s.notify.bar, s.notify.off + q->notify * s.notifyMul, 1, &index);
}
/* 記述子の列 (n 個) を積む。flags[i] が 2 なら装置が書き込む側 */
static int push(Queue *q, int n, const u64 *addr, const u32 *len, const u16 *wflag) {
    if (q->nfree < n) return -1;
    u16 ids[4];
    for (int i = 0; i < n; i++) ids[i] = q->free[--q->nfree];
    for (int i = 0; i < n; i++) {
        q->d[ids[i]].addr = addr[i]; q->d[ids[i]].len = len[i];
        q->d[ids[i]].flags = wflag[i] | (i + 1 < n ? 1 : 0);
        q->d[ids[i]].next = i + 1 < n ? ids[i + 1] : 0;
    }
    q->avail->ring[q->avail->idx % q->size] = ids[0];
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return ids[0];
}
/* 使い終わった列を回収する。回収した数を返す */
static int reclaim(Queue *q) {
    int n = 0;
    while (q->seen != q->used->idx) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        u16 id = q->used->ring[q->seen % q->size].id % q->size;
        for (;;) {
            u16 f = q->d[id].flags, next = q->d[id].next;
            q->free[q->nfree++] = id;
            if (!(f & 1)) break;
            id = next;
        }
        q->seen++; n++;
    }
    return n;
}

/* 制御要求: cbuf の先頭 len バイトを送り、返事を cbuf+2048 に受ける。返事の code を返す */
static u32 control(u32 len, u32 rlen) {
    u64 a[2] = {s.cdev, s.cdev + 2048}; u32 l[2] = {len, rlen}; u16 w[2] = {0, 2};
    memset(s.cbuf + 2048, 0, rlen);
    if (push(&s.ctl, 2, a, l, w) < 0) return 0;
    kick(&s.ctl, 0);
    for (int i = 0; i < 20000; i++) { if (reclaim(&s.ctl)) return *(u32 *)(s.cbuf + 2048); BS->Stall(50); }
    return 0;
}
static u32 pcm_cmd(u32 code) {
    u32 *r = (u32 *)s.cbuf; r[0] = code; r[1] = s.stream;
    return control(8, 4);
}

#define OK 0x8000
static int start(PciIo *io) {
    memset(&s, 0, sizeof s);
    s.io = io;
    if (cfg(0, W32) != 0x10591AF4) return 0; /* virtio-sound */
    find_regions();
    if (!s.common.found || !s.notify.found || !s.device.found) return 0;
    if (io->Attributes(io, 2, 0x200 | 0x400, 0)) return 0;
    Region *c = &s.common;
    wr(c, 20, W8, 0);
    for (int i = 0; i < 1000 && rd(c, 20, W8); i++) BS->Stall(100);
    wr(c, 20, W8, 1); wr(c, 20, W8, 1 | 2);
    wr(c, 0, W32, 1);
    if (!(rd(c, 4, W32) & 1)) return 0; /* VERSION_1 */
    wr(c, 8, W32, 0); wr(c, 12, W32, 0);
    wr(c, 8, W32, 1); wr(c, 12, W32, 1);
    wr(c, 20, W8, 1 | 2 | 8);
    if (!(rd(c, 20, W8) & 8)) return 0;
    if (!queue_setup(&s.ctl, 0) || !queue_setup(&s.tx, 2)) return 0;
    if (!(s.cbuf = dma(1, &s.cdev))) return 0;
    wr(c, 20, W8, 1 | 2 | 8 | 4); /* DRIVER_OK */

    /* 出力用の流れ (stream) を探す */
    u32 streams = rd(&s.device, 4, W32);
    if (streams > 8) streams = 8;
    u32 *q = (u32 *)s.cbuf;
    q[0] = 0x0100; q[1] = 0; q[2] = streams; q[3] = 32;
    if (!streams || control(16, 4 + 32 * streams) != OK) return 0;
    int found = 0;
    for (u32 i = 0; i < streams && !found; i++) {
        u8 *info = s.cbuf + 2048 + 4 + 32 * i;
        u64 formats = *(u64 *)(info + 8), rates = *(u64 *)(info + 16);
        if (info[24] == 0 && (formats & (1 << 5)) && (rates & (1 << 7)) && info[26] >= 2) s.stream = i, found = 1;
    }
    if (!found) return 0;
    q[0] = 0x0101; q[1] = s.stream; q[2] = FRAMES * 4; q[3] = PERIOD * 4; q[4] = 0;
    s.cbuf[20] = 2; s.cbuf[21] = 5; s.cbuf[22] = 7; s.cbuf[23] = 0; /* 2ch, S16, 48000Hz */
    if (control(24, 4) != OK) return 0;
    if (pcm_cmd(0x0102) != OK) return 0; /* PREPARE */
    u64 pages = ((u64)FRAMES * 4 + 4095) / 4096 + 1;
    if (!(s.pcm = dma(pages, &s.pdev))) return 0;
    return 1;
}

/* ---- 合成 ---- */
static double msin(double x) { /* -π..π に寄せてからテイラー展開 */
    const double PI = 3.14159265358979323846;
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;
    double x2 = x * x, t = x, r = x;
    for (int k = 1; k < 12; k++) { t *= -x2 / ((2 * k) * (2 * k + 1)); r += t; }
    return r;
}
static double mexp(double x) { /* x <= 0 の範囲で使う */
    if (x < -40) return 0;
    int n = 0; while (x < -0.5) x += 0.5, n++;
    double t = 1, r = 1;
    for (int k = 1; k < 14; k++) { t *= x / k; r += t; }
    while (n--) r *= 0.60653065971263342; /* e^-0.5 */
    return r;
}

static float mixL[FRAMES], mixR[FRAMES];
/* 正弦波を足す。回転で作るので速い */
static void partial(double hz, double amp, double tau, double atk, int at, double pan) {
    double w = 2 * 3.14159265358979323846 * hz / RATE;
    double c = msin(w + 3.14159265358979323846 / 2), sn = msin(w), x = 1, y = 0;
    double decay = mexp(-1.0 / (tau * RATE)), env = 1;
    double gl = amp * (1 - pan), gr = amp * pan;
    int na = (int)(atk * RATE);
    for (int i = at; i < FRAMES; i++) {
        double a = i - at < na ? (double)(i - at) / na : 1;
        double v = y * env * a;
        mixL[i] += (float)(v * gl); mixR[i] += (float)(v * gr);
        double nx = x * c - y * sn; y = x * sn + y * c; x = nx;
        env *= decay;
        if (((i - at) & 1023) == 0) { double m = x * x + y * y; m = (3 - m) / 2; x *= m; y *= m; } /* ずれを直す */
        if (env < 1e-5) break;
    }
}
static void bell(double hz, double amp, int ms, double pan) {
    int at = RATE * ms / 1000;
    double tau = 1.3 * mexp(-0.3 * (hz / 440 - 1) * 0.69); /* 高い音ほど早く消える */
    partial(hz, amp, tau, 0.004, at, pan);
    partial(hz * 1.0032, amp * 0.45, tau * 0.9, 0.006, at, 1 - pan); /* わずかにずらして広がりを出す */
    partial(hz * 2.0, amp * 0.28, tau * 0.45, 0.003, at, pan);
    partial(hz * 3.01, amp * 0.10, tau * 0.25, 0.002, at, 1 - pan);
    partial(hz * 4.2, amp * 0.04, tau * 0.12, 0.001, at, pan);
}
static void synth(void) {
    memset(mixL, 0, sizeof mixL); memset(mixR, 0, sizeof mixR);
    /* 低い支え (D3) をゆっくり立ち上げる */
    partial(146.83, 0.20, 1.6, 0.25, 0, 0.5);
    partial(220.00, 0.10, 1.4, 0.30, 0, 0.5);
    /* D メジャー add9 を下から順に */
    static const struct { double hz, amp; int ms; double pan; } n[] = {
        {293.66, 0.26, 0, 0.30}, {440.00, 0.22, 85, 0.42}, {587.33, 0.19, 170, 0.55},
        {739.99, 0.16, 255, 0.65}, {880.00, 0.14, 340, 0.72}, {1318.51, 0.09, 430, 0.80},
    };
    for (unsigned i = 0; i < sizeof n / sizeof n[0]; i++) bell(n[i].hz, n[i].amp, n[i].ms, n[i].pan);
    /* 簡単な残響: 左右で長さの違う遅延を重ねる */
    static const int dl[] = {1601, 2311, 3203}, dr[] = {1753, 2503, 3407};
    for (int i = FRAMES - 1; i >= 0; i--) {
        float l = 0, r = 0;
        for (int k = 0; k < 3; k++) {
            if (i >= dl[k]) l += mixR[i - dl[k]] * (0.22f - 0.05f * k);
            if (i >= dr[k]) r += mixL[i - dr[k]] * (0.22f - 0.05f * k);
        }
        mixL[i] += l; mixR[i] += r;
    }
    float peak = 0.001f;
    for (int i = 0; i < FRAMES; i++) {
        float a = mixL[i] < 0 ? -mixL[i] : mixL[i], b = mixR[i] < 0 ? -mixR[i] : mixR[i];
        if (a > peak) peak = a;
        if (b > peak) peak = b;
    }
    float g = 0.55f * 32767 / peak;
    int fade = RATE / 4;
    for (int i = 0; i < FRAMES; i++) {
        float f = i > FRAMES - fade ? (float)(FRAMES - i) / fade : 1;
        s.pcm[i * 2] = (i16)(mixL[i] * g * f);
        s.pcm[i * 2 + 1] = (i16)(mixR[i] * g * f);
    }
}

/* 送れるだけ送る */
static void feed(void) {
    while (s.sent < NPERIOD) {
        u32 *hdr = (u32 *)(s.cbuf + 1024 + 16 * (s.sent % 32)); /* 0..1023 は制御用 */
        hdr[0] = s.stream;
        u64 a[3] = {s.cdev + 1024 + 16 * (s.sent % 32), s.pdev + (u64)s.sent * PERIOD * 4, s.cdev + 3072 + 16 * (s.sent % 32)};
        u32 l[3] = {4, PERIOD * 4, 8}; u16 w[3] = {0, 0, 2};
        if (push(&s.tx, 3, a, l, w) < 0) break;
        s.sent++;
    }
    kick(&s.tx, 2);
}

void sound_init(void) {
    Guid g = {0x4cf5b200, 0x68b8, 0x4ca5, {0x9e,0xec,0xb2,0x3e,0x3f,0x50,0x02,0x9a}};
    u64 n; Handle *h;
    if (BS->LocateHandleBuffer(2, &g, 0, &n, &h)) return;
    for (u64 i = 0; i < n && !s.ready; i++) {
        PciIo *io;
        if (!BS->HandleProtocol(h[i], &g, (void **)&io)) s.ready = start(io);
    }
    BS->FreePool(h);
    if (!s.ready && (s.pcm = hda_init(FRAMES + RATE / 2))) s.ready = s.hda = 1; /* 後ろ 0.5 秒は無音 (止めるのが遅れても繰り返さない) */
}

void sound_chime(void) {
    if (!s.ready || s.playing) return;
    synth();
    if (s.hda) { hda_start(); s.playing = 1; s.lastPos = 0; s.hdaEnd = net_ms() + SECONDS * 1000 + 400; return; }
    s.playing = 1; s.sent = 0; s.done = 0;
    feed();
    pcm_cmd(0x0104); /* START */
}

void sound_poll(void) {
    if (!s.playing) return;
    if (s.hda) {
        /* 繰り返し再生されるので、鳴らし終えて後ろの無音に入ったら止める。念のため時間でも止める */
        u32 pos = hda_pos();
        if (pos >= (u32)(FRAMES + RATE * 2 / 5) * 4 || pos < s.lastPos || net_ms() >= s.hdaEnd) hda_stop(), s.playing = 0;
        s.lastPos = pos;
        return;
    }
    s.done += reclaim(&s.tx);
    if (s.sent < NPERIOD) feed();
    if (s.done >= NPERIOD) {
        pcm_cmd(0x0105); /* STOP */
        pcm_cmd(0x0103); /* RELEASE */
        s.playing = 0;
    }
}
