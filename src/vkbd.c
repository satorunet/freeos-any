/* 仮想マシン用のキーボード (virtio-keyboard) を直接読む。
 * UEFI のキーボード処理は Mac の「かな」「英数」(QEMU では 変換・無変換) を捨ててしまうので、
 * 仮想マシンではこちらで全部のキーを受け取り、UEFI と同じ形 (Key) にして渡す。US 配列。 */
#include "os.h"
#include "vkbd.h"

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
    PciIo *io; Region common, notify, device; u32 notifyMul, notifyOff;
    volatile Avail *avail; volatile Used *used; volatile InputEvent *ev;
    u16 size, seen; int ready, shift, ctrl, caps;
} t;
static u32 rd(Region *r, u32 off, int w) { u32 v = 0; t.io->MemRead(t.io, w, r->bar, r->off + off, 1, &v); return v; }
static void wr(Region *r, u32 off, int w, u32 v) { t.io->MemWrite(t.io, w, r->bar, r->off + off, 1, &v); }
static u32 cfg(u32 off, int w) { u32 v = 0; t.io->CfgRead(t.io, w, off, 1, &v); return v; }
static void kick(void) { u16 q = 0; t.io->MemWrite(t.io, W16, t.notify.bar, t.notify.off + t.notifyOff * t.notifyMul, 1, &q); }

static int start(PciIo *io) {
    t.io = io; t.common.found = t.notify.found = t.device.found = 0;
    if (cfg(0, W32) != 0x10521AF4) return 0; /* virtio-input */
    if (!(cfg(0x06, W16) & 0x10)) return 0;
    u32 p = cfg(0x34, W8) & 0xFC;
    for (int n = 0; p && n < 48; n++, p = cfg(p + 1, W8) & 0xFC) {
        if (cfg(p, W8) != 0x09) continue;
        u32 type = cfg(p + 3, W8);
        Region *r = type == 1 ? &t.common : type == 2 ? &t.notify : type == 4 ? &t.device : 0;
        if (!r || r->found) continue;
        r->bar = cfg(p + 4, W8); r->off = cfg(p + 8, W32); r->found = 1;
        if (type == 2) t.notifyMul = cfg(p + 16, W32);
    }
    if (!t.common.found || !t.notify.found || !t.device.found) return 0;
    /* キーボードか: EV_KEY に KEY_A (30) があり、絶対座標 (タブレット) はない */
    wr(&t.device, 0, W8, 0x11); wr(&t.device, 1, W8, 1);
    if (rd(&t.device, 2, W8) < 4 || !(rd(&t.device, 8 + 30 / 8, W8) & (1 << (30 % 8)))) return 0;
    wr(&t.device, 0, W8, 0x11); wr(&t.device, 1, W8, 3);
    if (rd(&t.device, 2, W8)) return 0;
    if (io->Attributes(io, 2, 0x200 | 0x400, 0)) return 0;
    Region *c = &t.common;
    wr(c, 20, W8, 0); for (int i = 0; i < 1000 && rd(c, 20, W8); i++) BS->Stall(100);
    wr(c, 20, W8, 1); wr(c, 20, W8, 1 | 2);
    wr(c, 0, W32, 1); u32 high = rd(c, 4, W32); if (!(high & 1)) return 0;
    wr(c, 8, W32, 0); wr(c, 12, W32, 0); wr(c, 8, W32, 1); wr(c, 12, W32, high & 3);
    wr(c, 20, W8, 1 | 2 | 8); if (!(rd(c, 20, W8) & 8)) return 0;
    wr(c, 22, W16, 0);
    u32 size = rd(c, 24, W16); if (!size) return 0; if (size > QSIZE) size = QSIZE; if (size & (size - 1)) return 0;
    void *mem, *map; u64 bytes = 4096, dev;
    if (io->AllocateBuffer(io, 0, 4, 1, &mem, 0)) return 0;
    if (io->Map(io, 2, mem, &bytes, &dev, &map) || bytes < 4096) return 0;
    memset(mem, 0, 4096);
    Desc *d = mem;
    t.avail = (void *)((u8 *)mem + 1024); t.used = (void *)((u8 *)mem + 1536); t.ev = (void *)((u8 *)mem + 2560);
    t.size = size; t.seen = 0;
    for (u32 i = 0; i < size; i++) { d[i].addr = dev + 2560 + i * sizeof(InputEvent); d[i].len = sizeof(InputEvent); d[i].flags = 2; t.avail->ring[i] = i; }
    wr(c, 24, W16, size);
    wr(c, 32, W32, (u32)dev); wr(c, 36, W32, (u32)(dev >> 32));
    wr(c, 40, W32, (u32)(dev + 1024)); wr(c, 44, W32, (u32)((dev + 1024) >> 32));
    wr(c, 48, W32, (u32)(dev + 1536)); wr(c, 52, W32, (u32)((dev + 1536) >> 32));
    t.notifyOff = rd(c, 30, W16);
    wr(c, 28, W16, 1);
    wr(c, 20, W8, 1 | 2 | 8 | 4);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    t.avail->idx = size;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    kick();
    return 1;
}
int vkbd_init(void) {
    Guid g = {0x4cf5b200, 0x68b8, 0x4ca5, {0x9e,0xec,0xb2,0x3e,0x3f,0x50,0x02,0x9a}};
    u64 n; Handle *h;
    if (BS->LocateHandleBuffer(2, &g, 0, &n, &h)) return 0;
    for (u64 i = 0; i < n && !t.ready; i++) { PciIo *io; if (!BS->HandleProtocol(h[i], &g, (void **)&io)) t.ready = start(io); }
    BS->FreePool(h);
    return t.ready;
}

/* Linux のキー番号 → 文字 (US 配列)。[通常, Shift] */
static const char keymap[][2] = {
    [2] = {'1', '!'}, [3] = {'2', '@'}, [4] = {'3', '#'}, [5] = {'4', '$'}, [6] = {'5', '%'}, [7] = {'6', '^'}, [8] = {'7', '&'},
    [9] = {'8', '*'}, [10] = {'9', '('}, [11] = {'0', ')'}, [12] = {'-', '_'}, [13] = {'=', '+'}, [15] = {'\t', '\t'},
    [16] = {'q', 'Q'}, [17] = {'w', 'W'}, [18] = {'e', 'E'}, [19] = {'r', 'R'}, [20] = {'t', 'T'}, [21] = {'y', 'Y'}, [22] = {'u', 'U'},
    [23] = {'i', 'I'}, [24] = {'o', 'O'}, [25] = {'p', 'P'}, [26] = {'[', '{'}, [27] = {']', '}'}, [28] = {13, 13},
    [30] = {'a', 'A'}, [31] = {'s', 'S'}, [32] = {'d', 'D'}, [33] = {'f', 'F'}, [34] = {'g', 'G'}, [35] = {'h', 'H'}, [36] = {'j', 'J'},
    [37] = {'k', 'K'}, [38] = {'l', 'L'}, [39] = {';', ':'}, [40] = {'\'', '"'}, [41] = {'`', '~'}, [43] = {'\\', '|'},
    [44] = {'z', 'Z'}, [45] = {'x', 'X'}, [46] = {'c', 'C'}, [47] = {'v', 'V'}, [48] = {'b', 'B'}, [49] = {'n', 'N'}, [50] = {'m', 'M'},
    [51] = {',', '<'}, [52] = {'.', '>'}, [53] = {'/', '?'}, [57] = {' ', ' '}, [89] = {'\\', '_'}, [124] = {'\\', '|'},
    [14] = {8, 8},
};
/* Linux のキー番号 → UEFI のスキャンコード */
static u16 scan_of(u16 code) {
    switch (code) {
    case 103: return 1; case 108: return 2; case 106: return 3; case 105: return 4; case 102: return 5; case 107: return 6;
    case 110: return 7; case 111: return 8; case 104: return 9; case 109: return 10; case 1: return 0x17;
    case 87: return 0x15; case 88: return 0x16;
    }
    if (code >= 59 && code <= 68) return 0x0B + (code - 59); /* F1〜F10 */
    return 0;
}
int vkbd_read(Key *k, int *special) {
    *special = 0;
    if (!t.ready) return 0;
    int got = 0;
    while (!got && t.seen != t.used->idx) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        u32 id = t.used->ring[t.seen % t.size].id % t.size;
        InputEvent e = t.ev[id];
        t.avail->ring[t.avail->idx % t.size] = id;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        t.avail->idx++; t.seen++;
        kick();
        if (e.type != 1) continue; /* EV_KEY だけ */
        u16 c = e.code; int down = e.value != 0;
        if (c == 42 || c == 54) { t.shift = down; continue; }
        if (c == 29 || c == 97) { t.ctrl = down; continue; }
        if (!down) continue;
        if (c == 58) { t.caps ^= 1; continue; }
        if (c == 92 || c == 93 || c == 122 || c == 90) { *special = 1; return 1; } /* 変換・カタカナひらがな・LANG1 →「かな」 */
        if (c == 94 || c == 123 || c == 85) { *special = 2; return 1; }            /* 無変換・LANG2・半角全角 →「英数」 */
        k->scan = scan_of(c); k->ch = 0;
        if (!k->scan && c < sizeof keymap / sizeof *keymap && keymap[c][0]) {
            char ch = keymap[c][t.shift ? 1 : 0];
            if (t.caps && ch >= 'a' && ch <= 'z') ch -= 32; else if (t.caps && ch >= 'A' && ch <= 'Z') ch += 32;
            if (t.ctrl && ((ch | 32) >= 'a' && (ch | 32) <= 'z')) ch = (char)((ch | 32) - 'a' + 1);
            k->ch = (c16)(u8)ch;
        }
        if (k->scan || k->ch) got = 1;
    }
    return got;
}
