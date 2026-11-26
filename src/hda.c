/* 実機用の音声ドライバ: Intel High Definition Audio (HDA)。
 * ほとんどの PC (Intel / AMD) の内蔵音声はこの方式。PCI を直接操作する。
 * コーデック内の部品のつながりを調べて、スピーカー・ヘッドホン・ライン出力から DAC までの道をつなぐ。 */
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

static struct {
    PciIo *io;
    volatile u32 *corb; volatile u64 *rirb; u64 corbDev, rirbDev;
    u16 rirbRp;
    u32 sd;            /* 出力ストリームのレジスタ位置 */
    u64 *bdl; u64 bdlDev;
    void *pcm; u64 pcmDev; u32 bytes;
    int ok;
} h;

static u32 r32(u32 off) { u32 v = 0; h.io->MemRead(h.io, W32, 0, off, 1, &v); return v; }
static u32 r16(u32 off) { u32 v = 0; h.io->MemRead(h.io, W16, 0, off, 1, &v); return v; }
static u32 r8(u32 off) { u32 v = 0; h.io->MemRead(h.io, W8, 0, off, 1, &v); return v; }
static void w32(u32 off, u32 v) { h.io->MemWrite(h.io, W32, 0, off, 1, &v); }
static void w16(u32 off, u32 v) { h.io->MemWrite(h.io, W16, 0, off, 1, &v); }
static void w8(u32 off, u32 v) { h.io->MemWrite(h.io, W8, 0, off, 1, &v); }
static u32 cfg(u32 off, int w) { u32 v = 0; h.io->CfgRead(h.io, w, off, 1, &v); return v; }
/* 条件がそろうまで待つ (最大 ms ミリ秒) */
#define WAIT(cond, ms) ({ int _i = 0; for (; _i < (ms) * 10 && !(cond); _i++) BS->Stall(100); (cond); })

static void *dma(u64 pages, u64 *dev) {
    void *mem, *map; u64 bytes = pages * 4096;
    if (h.io->AllocateBuffer(h.io, 0, 4, pages, &mem, 0)) return 0;
    if (h.io->Map(h.io, 2, mem, &bytes, dev, &map) || bytes < pages * 4096) return 0;
    memset(mem, 0, pages * 4096);
    return mem;
}

/* ---- コーデックへの命令 (CORB で送り RIRB で受ける) ---- */
static int cad;
static u32 cmd(u32 nid, u32 verb) {
    u32 wp = (r16(0x48) + 1) & 0xFF;
    h.corb[wp] = (u32)cad << 28 | nid << 20 | verb;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    w16(0x48, wp);
    u16 before = h.rirbRp;
    if (!WAIT((r16(0x58) & 0xFF) != before, 50)) return 0xFFFFFFFF;
    h.rirbRp = (before + 1) & 0xFF;
    w8(0x5D, 5); /* 「応答あり」を消す (消すまで次の命令を処理しない装置がある) */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return (u32)h.rirb[h.rirbRp];
}
static u32 param(u32 nid, u32 p) { return cmd(nid, 0xF00 << 8 | p); }
static u32 verb12(u32 nid, u32 v, u32 payload) { return cmd(nid, v << 8 | (payload & 0xFF)); }
static u32 verb4(u32 nid, u32 v, u32 payload) { return cmd(nid, v << 16 | (payload & 0xFFFF)); }

/* ---- 部品のつながり ---- */
#define MAXN 128
static u8 type_[MAXN], nconn[MAXN], conn[MAXN][16];
static u32 caps[MAXN];
static int first, count, afg;

static void read_conn(u32 nid) {
    u32 len = param(nid, 0x0E), longForm = len & 0x80, n = len & 0x7F, got = 0;
    if (n > 16) n = 16;
    for (u32 i = 0; i < n; i += longForm ? 2 : 4) {
        u32 v = verb12(nid, 0xF02, i);
        for (u32 j = 0; j < (longForm ? 2u : 4u) && i + j < n; j++) {
            u32 e = longForm ? (v >> (16 * j)) & 0xFFFF : (v >> (8 * j)) & 0xFF;
            conn[nid][got++] = (u8)(e & (longForm ? 0x7FFF : 0x7F));
        }
    }
    nconn[nid] = (u8)got;
}
/* nid から DAC までの道を探す。見つかれば path に並べて長さを返す */
static int find_path(u32 nid, u8 *path, int depth) {
    if (nid < (u32)first || nid >= (u32)(first + count) || depth > 5) return 0;
    path[depth] = (u8)nid;
    if (type_[nid] == 0) return depth + 1; /* DAC */
    if (type_[nid] != 2 && type_[nid] != 3 && type_[nid] != 4) return 0;
    for (int i = 0; i < nconn[nid]; i++) {
        int n = find_path(conn[nid][i], path, depth + 1);
        if (n) {
            if (nconn[nid] > 1 && type_[nid] != 2) verb12(nid, 0x701, i); /* つなぐ先を選ぶ */
            if (type_[nid] == 2) verb4(nid, 0x3, 0x7000 | i << 8);         /* ミキサーの入力の消音を解く */
            return n;
        }
    }
    return 0;
}
/* 出力側の音量を 0dB 付近にして消音を解く */
static void unmute_out(u32 nid) {
    u32 ac = param(nid, 0x12);
    if (!(caps[nid] & (1 << 3))) ac = param(afg, 0x12); /* 部品が持たないときは全体の設定を使う */
    u32 gain = ac & 0x7F;
    verb4(nid, 0x3, 0xB000 | gain);
}

static int setup_codec(void) {
    u32 sub = param(0, 0x04), fg = (sub >> 16) & 0xFF, nfg = sub & 0xFF;
    afg = 0;
    for (u32 i = 0; i < nfg; i++) if ((param(fg + i, 0x05) & 0xFF) == 1) { afg = fg + i; break; }
    if (!afg) return 0;
    verb12(afg, 0x705, 0); /* 電源 D0 */
    BS->Stall(10000);
    u32 ws = param(afg, 0x04);
    first = (ws >> 16) & 0xFF; count = ws & 0xFF;
    if (first + count > MAXN) count = MAXN - first;
    for (int n = first; n < first + count; n++) {
        caps[n] = param(n, 0x09);
        type_[n] = (caps[n] >> 20) & 0xF;
        nconn[n] = 0;
        if (caps[n] & (1 << 8)) read_conn(n);
    }
    /* つながっている出力端子 (ライン出力 0 / スピーカー 1 / ヘッドホン 2) すべてに道をつなぐ */
    int dacs = 0; u8 used[MAXN] = {0};
    for (int n = first; n < first + count; n++) {
        if (type_[n] != 4) continue;
        u32 def = verb12(n, 0xF1C, 0), dev = (def >> 20) & 0xF, port = def >> 30;
        if (dev > 2 || port == 1) continue; /* つながっていない端子は飛ばす */
        u8 path[8];
        int len = find_path(n, path, 0);
       
        if (!len) continue;
        for (int i = 0; i < len; i++) {
            u32 nid = path[i];
            verb12(nid, 0x705, 0);
            if (caps[nid] & (1 << 2)) unmute_out(nid);
            if (type_[nid] == 0 && !used[nid]) {
                verb12(nid, 0x706, 1 << 4); /* ストリーム 1、チャンネル 0 から */
                verb4(nid, 0x2, 0x0011);    /* 48kHz 16ビット 2ch */
                used[nid] = 1; dacs++;
            }
        }
        verb12(n, 0x707, dev == 2 ? 0xC0 : 0x40); /* 出力を有効に (ヘッドホンは増幅も) */
        if (param(n, 0x0C) & (1 << 16)) verb12(n, 0x70C, 2); /* 外部アンプ (EAPD) を入れる */
    }
    return dacs > 0;
}

static int start(PciIo *io, u32 frames) {
    memset(&h, 0, sizeof h);
    h.io = io;
    if (cfg(0x08, W32) >> 16 != 0x0403) return 0; /* 音声 / HDA */
    if (io->Attributes(io, 2, 0x200 | 0x400, 0)) return 0;
    /* 装置をリセット */
    w32(0x08, r32(0x08) & ~1u);
    WAIT(!(r32(0x08) & 1), 100);
    w32(0x08, r32(0x08) | 1);
    if (!WAIT(r32(0x08) & 1, 100)) return 0;
    BS->Stall(2000);
    u32 codecs = r16(0x0E);
    if (!codecs) return 0;
    /* CORB と RIRB */
    u64 dev; u8 *mem = dma(1, &dev);
    if (!mem) return 0;
    h.corb = (u32 *)mem; h.rirb = (u64 *)(mem + 2048); h.corbDev = dev; h.rirbDev = dev + 2048;
    w8(0x4C, 0); w8(0x5C, 0);
    WAIT(!(r8(0x4C) & 2) && !(r8(0x5C) & 2), 50);
    w32(0x40, (u32)h.corbDev); w32(0x44, (u32)(h.corbDev >> 32));
    w8(0x4E, 2); /* 256 個 */
    w16(0x48, 0);
    w16(0x4A, 0x8000); WAIT(r16(0x4A) & 0x8000, 10); w16(0x4A, 0); WAIT(!(r16(0x4A) & 0x8000), 10);
    w32(0x50, (u32)h.rirbDev); w32(0x54, (u32)(h.rirbDev >> 32));
    w8(0x5E, 2);
    w16(0x58, 0x8000);
    w16(0x5A, 1);
    w8(0x4C, 2); w8(0x5C, 3); /* RIRB は「応答あり」を立てる設定にする (CPU への割り込みは全体で切ってある) */
    h.rirbRp = 0;
    int any = 0;
    for (cad = 0; cad < 15 && !any; cad++) if (codecs & (1 << cad)) any = setup_codec();
    if (!any) return 0;
    /* 最初の出力ストリームを使う */
    u32 gcap = r16(0x00), iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) return 0;
    h.sd = 0x80 + iss * 0x20;
    w8(h.sd, 1); WAIT(r8(h.sd) & 1, 10); w8(h.sd, 0); WAIT(!(r8(h.sd) & 1), 10);
    h.bytes = frames * 4;
    u64 pages = (h.bytes + 4095) / 4096;
    if (!(h.pcm = dma(pages, &h.pcmDev))) return 0;
    if (!(h.bdl = dma(1, &h.bdlDev))) return 0;
    /* 1つ 64KB ずつに分けて並べる */
    u32 n = 0;
    for (u32 off = 0; off < h.bytes && n < 255; n++, off += 65536) {
        u32 len = h.bytes - off < 65536 ? h.bytes - off : 65536;
        h.bdl[n * 2] = h.pcmDev + off;
        h.bdl[n * 2 + 1] = len;
    }
    w32(h.sd + 0x08, h.bytes);
    w16(h.sd + 0x0C, n - 1);
    w16(h.sd + 0x12, 0x0011);
    w32(h.sd + 0x18, (u32)h.bdlDev); w32(h.sd + 0x1C, (u32)(h.bdlDev >> 32));
    w8(h.sd + 2, 1 << 4); /* ストリーム番号 1 */
    return 1;
}

/* 見つかれば音のデータを書き込む場所を返す (48kHz 16ビット ステレオ、frames 個) */
i16 *hda_init(u32 frames) {
    Guid g = {0x4cf5b200, 0x68b8, 0x4ca5, {0x9e,0xec,0xb2,0x3e,0x3f,0x50,0x02,0x9a}};
    u64 n; Handle *hs;
    if (BS->LocateHandleBuffer(2, &g, 0, &n, &hs)) return 0;
    for (u64 i = 0; i < n && !h.ok; i++) {
        PciIo *io;
        if (!BS->HandleProtocol(hs[i], &g, (void **)&io)) h.ok = start(io, frames);
    }
    BS->FreePool(hs);
    return h.ok ? h.pcm : 0;
}
void hda_start(void) { if (h.ok) w8(h.sd, r8(h.sd) | 2); }
/* 止める (繰り返し再生されるので、時間で止める) */
/* 今どこまで読んだか (バイト) */
u32 hda_pos(void) { return h.ok ? r32(h.sd + 0x04) : 0; }
void hda_stop(void) { if (h.ok) w8(h.sd, r8(h.sd) & ~2u); }
