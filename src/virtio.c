/* 仮想マシン用のタブレット(virtio-input)ドライバ。
 * UEFI ファームウェアは多くの場合この装置を扱わないので、PCI を直接操作する (virtio 1.0)。
 * Mac / Windows / Linux の上で動かすとき、これでマウスが使える。実機には影響しない。 */
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

#define QSIZE 64
typedef struct { u64 addr; u32 len; u16 flags, next; } Desc;
typedef struct { u16 flags, idx, ring[QSIZE]; } Avail;
typedef struct { u16 flags, idx; struct { u32 id, len; } ring[QSIZE]; } Used;
typedef struct { u16 type, code; u32 value; } InputEvent;

static struct {
    PciIo *io;
    Region common, notify, device;
    u32 notifyMul, notifyOff;
    volatile Avail *avail;
    volatile Used *used;
    volatile InputEvent *ev;
    u16 size, seen;
    int x, y, down, ready;
} t;

static u32 rd(Region *r, u32 off, int w) {
    u32 v = 0;
    t.io->MemRead(t.io, w, r->bar, r->off + off, 1, &v);
    return v;
}
static void wr(Region *r, u32 off, int w, u32 v) { t.io->MemWrite(t.io, w, r->bar, r->off + off, 1, &v); }
static u32 cfg(u32 off, int w) {
    u32 v = 0;
    t.io->CfgRead(t.io, w, off, 1, &v);
    return v;
}
static void kick(void) {
    u16 q = 0;
    t.io->MemWrite(t.io, W16, t.notify.bar, t.notify.off + t.notifyOff * t.notifyMul, 1, &q);
}

static void find_regions(void) {
    t.common.found = t.notify.found = t.device.found = 0;
    if (!(cfg(0x06, W16) & 0x10)) return;
    u32 p = cfg(0x34, W8) & 0xFC;
    for (int n = 0; p && n < 48; n++, p = cfg(p + 1, W8) & 0xFC) {
        if (cfg(p, W8) != 0x09) continue; /* ベンダー固有の項目だけ見る */
        u32 type = cfg(p + 3, W8);
        Region *r = type == 1 ? &t.common : type == 2 ? &t.notify : type == 4 ? &t.device : 0;
        if (!r || r->found) continue;
        r->bar = cfg(p + 4, W8); r->off = cfg(p + 8, W32); r->found = 1;
        if (type == 2) t.notifyMul = cfg(p + 16, W32);
    }
}

static int start(PciIo *io) {
    t.io = io;
    if (cfg(0, W32) != 0x10521AF4) return 0; /* virtio-input */
    find_regions();
    if (!t.common.found || !t.notify.found || !t.device.found) return 0;
    /* 絶対座標を持つ装置(タブレット)だけを使う。キーボードには触らない */
    wr(&t.device, 0, W8, 0x11); wr(&t.device, 1, W8, 3);
    if (!rd(&t.device, 2, W8)) return 0;
    if (io->Attributes(io, 2, 0x200 | 0x400, 0)) return 0; /* メモリとバスマスタを有効に */

    Region *c = &t.common;
    wr(c, 20, W8, 0);
    for (int i = 0; i < 1000 && rd(c, 20, W8); i++) BS->Stall(100);
    wr(c, 20, W8, 1);
    wr(c, 20, W8, 1 | 2);
    wr(c, 0, W32, 1);
    u32 high = rd(c, 4, W32);
    if (!(high & 1)) return 0; /* VERSION_1 */
    wr(c, 8, W32, 0); wr(c, 12, W32, 0);
    wr(c, 8, W32, 1); wr(c, 12, W32, high & 3);
    wr(c, 20, W8, 1 | 2 | 8);
    if (!(rd(c, 20, W8) & 8)) return 0;

    wr(c, 22, W16, 0);
    u32 size = rd(c, 24, W16);
    if (!size) return 0;
    if (size > QSIZE) size = QSIZE;
    if (size & (size - 1)) return 0;
    void *mem, *map;
    u64 bytes = 4096, dev;
    if (io->AllocateBuffer(io, 0, 4, 1, &mem, 0)) return 0;
    if (io->Map(io, 2, mem, &bytes, &dev, &map) || bytes < 4096) return 0;
    memset(mem, 0, 4096);
    /* 1ページの中に: 記述子 0, avail 1024, used 1536, イベント 2560 */
    Desc *d = mem;
    t.avail = (void *)((u8 *)mem + 1024);
    t.used = (void *)((u8 *)mem + 1536);
    t.ev = (void *)((u8 *)mem + 2560);
    t.size = size; t.seen = 0;
    for (u32 i = 0; i < size; i++) {
        d[i].addr = dev + 2560 + i * sizeof(InputEvent);
        d[i].len = sizeof(InputEvent);
        d[i].flags = 2; /* 装置が書き込む */
        t.avail->ring[i] = i;
    }
    wr(c, 24, W16, size);
    wr(c, 32, W32, (u32)dev); wr(c, 36, W32, (u32)(dev >> 32));
    wr(c, 40, W32, (u32)(dev + 1024)); wr(c, 44, W32, (u32)((dev + 1024) >> 32));
    wr(c, 48, W32, (u32)(dev + 1536)); wr(c, 52, W32, (u32)((dev + 1536) >> 32));
    t.notifyOff = rd(c, 30, W16);
    wr(c, 28, W16, 1);
    wr(c, 20, W8, 1 | 2 | 8 | 4); /* DRIVER_OK */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    t.avail->idx = size;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    kick();
    return 1;
}

void tablet_init(void) {
    Guid g = {0x4cf5b200, 0x68b8, 0x4ca5, {0x9e,0xec,0xb2,0x3e,0x3f,0x50,0x02,0x9a}};
    u64 n; Handle *h;
    if (BS->LocateHandleBuffer(2, &g, 0, &n, &h)) return;
    for (u64 i = 0; i < n && !t.ready; i++) {
        PciIo *io;
        if (!BS->HandleProtocol(h[i], &g, (void **)&io)) t.ready = start(io);
    }
    BS->FreePool(h);
}

void tablet_poll(void) {
    if (!t.ready) return;
    int used = 0;
    while (used < 256 && t.seen != t.used->idx) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        u32 id = t.used->ring[t.seen % t.size].id % t.size;
        InputEvent e = t.ev[id];
        if (e.type == 3 && e.code == 0) t.x = e.value;
        else if (e.type == 3 && e.code == 1) t.y = e.value;
        else if (e.type == 1 && (e.code == 0x110 || e.code == 0x14A)) t.down = e.value != 0;
        else if (e.type == 0) pointer_abs(t.x, t.y, t.down); /* 1回分の入力がそろった */
        t.avail->ring[t.avail->idx % t.size] = id; /* 同じ場所を装置に返す */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        t.avail->idx++;
        t.seen++;
        used++;
    }
    if (used) { __atomic_thread_fence(__ATOMIC_SEQ_CST); kick(); }
}
