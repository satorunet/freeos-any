/* AI チャット: Qwen3-0.6B (HOME\AI\qwen3-0.6b.fqw) と話す。OS の中だけで動き、通信はしない。
 * Enter で送る、F3 でローマ字かな入力、Delete で会話を忘れる、ホイールで読み返す */
#include "os.h"
#include "llm.h"
#include "net.h"

#define MODEL_PATH u"\\AI\\qwen3-0.6b.fqw"
#define LOGMAX 24000
#define MSGMAX 64
static c16 logbuf[LOGMAX]; static int loglen;
static struct { int start, len, user; } msg[MSGMAX]; static int nmsg;
static c16 input[400]; static int ilen, kanaMode = 1; /* 最初からかな入力 */
static int state; /* 0 未読み込み 1 読み込み待ち 2 準備できた 3 返事中 9 使えない */
static c16 note[120];
static int scrollUp; /* 下から何行ぶん戻っているか */
static u64 genStart; static int genTokens;
static char pend[8]; static int npend; /* 途中までの UTF-8 */

void *llm_alloc(u64 n) { void *p; if (BS->AllocatePool(2, n, &p)) return 0; memset(p, 0, n); return p; }

static void msg_new(int user) {
    if (nmsg == MSGMAX || loglen > LOGMAX - 2000) { /* 古いものを捨てる */
        int drop = msg[nmsg > 8 ? 8 : 1].start;
        mcopy(logbuf, logbuf + drop, (u64)(loglen - drop) * 2); loglen -= drop;
        int d = nmsg > 8 ? 8 : 1;
        for (int i = d; i < nmsg; i++) msg[i - d] = msg[i], msg[i - d].start -= drop;
        nmsg -= d;
    }
    msg[nmsg].start = loglen; msg[nmsg].len = 0; msg[nmsg].user = user; nmsg++;
}
static void msg_add(c16 c) { if (loglen < LOGMAX - 1 && nmsg) { logbuf[loglen++] = c; msg[nmsg - 1].len++; } }
static void msg_addz(const c16 *s) { while (*s) msg_add(*s++); }
static void in_add(c16 c) { if (ilen < 398) input[ilen++] = c; }

static void load_model(void) {
    Entry e;
    if (!fs_stat(MODEL_PATH, &e) || e.size < 1024) {
        state = 9;
        scopy(note, u"モデルがありません: HOME\\AI\\qwen3-0.6b.fqw を置いてください", 120);
        return;
    }
    u8 *buf;
    if (BS->AllocatePool(2, e.size, (void **)&buf)) { state = 9; scopy(note, u"メモリが足りません (仮想マシンは 1GB 以上に)", 120); return; }
    if (fs_read(MODEL_PATH, buf, e.size) != (i64)e.size) { state = 9; scopy(note, u"モデルを読めません", 120); return; }
    if (!llm_load(buf, e.size)) {
        state = 9; note[0] = 0;
        const char *m = llm_error(); int k = 0; u64 i = 0, n = 0; while (m[n]) n++;
        while (i < n && k < 118) { u32 c = (u8)m[i++]; if (c >= 0xE0) { c = (c & 15) << 12 | ((u8)m[i] & 63) << 6 | ((u8)m[i + 1] & 63); i += 2; } else if (c >= 0xC0) { c = (c & 31) << 6 | ((u8)m[i++] & 63); } note[k++] = (c16)c; }
        note[k] = 0; return;
    }
    state = 2;
    scopy(note, llm_has_avx2() ? u"準備できました (AVX2)" : u"準備できました (AVX2 なし: ゆっくり)", 120);
    if (!nmsg) { msg_new(0); msg_addz(u"こんにちは。Qwen3-0.6B です。この PC の中だけで動いています。何でも聞いてください。"); }
}
void chat_open(void) { if (state == 0) { state = 1; scopy(note, u"モデルを読み込んでいます… (数秒〜数十秒)", 120); } }

static void send(void) {
    kana_flush(in_add);
    if (!ilen || state != 2) return;
    msg_new(1); for (int i = 0; i < ilen; i++) msg_add(input[i]);
    static char u8buf[1400]; int n = 0;
    for (int i = 0; i < ilen && n < 1390; i++) {
        c16 c = input[i];
        if (c < 0x80) u8buf[n++] = (char)c;
        else if (c < 0x800) { u8buf[n++] = (char)(0xC0 | c >> 6); u8buf[n++] = (char)(0x80 | (c & 63)); }
        else { u8buf[n++] = (char)(0xE0 | c >> 12); u8buf[n++] = (char)(0x80 | ((c >> 6) & 63)); u8buf[n++] = (char)(0x80 | (c & 63)); }
    }
    ilen = 0; scrollUp = 0;
    scopy(note, u"考えています…", 120);
    redraw();
    if (!llm_begin(u8buf, n)) { scopy(note, u"長すぎます", 120); return; }
    msg_new(0);
    state = 3; genStart = net_ms(); genTokens = 0; npend = 0;
}
static void put_utf8(const char *s, int n) {
    for (int i = 0; i < n; i++) {
        pend[npend++] = s[i];
        u8 b0 = (u8)pend[0]; int need = b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : b0 >= 0xC0 ? 2 : 1;
        if (npend < need) continue;
        u32 c = need == 1 ? b0 : need == 2 ? (b0 & 31) << 6 | (pend[1] & 63) : need == 3 ? (b0 & 15) << 12 | (pend[1] & 63) << 6 | (pend[2] & 63) : 0xFFFD;
        npend = 0;
        if (c == '\r') continue;
        if (c > 0xFFFF) c = u'□';
        msg_add((c16)c);
    }
}
void chat_tick(void) {
    if (state == 1) { static int wait; if (++wait < 3) { invalidate(); return; } load_model(); invalidate(); return; }
    if (state != 3) return;
    u64 t0 = net_ms();
    while (net_ms() - t0 < 60) { /* 1 回に 60ms まで (画面を止めない) */
        char out[64]; int n = llm_step(out, 64);
        if (n < 0) {
            state = 2;
            while (nmsg && msg[nmsg - 1].len && (logbuf[msg[nmsg - 1].start + msg[nmsg - 1].len - 1] == u'\n')) msg[nmsg - 1].len--, loglen--;
            break;
        }
        put_utf8(out, n); genTokens++;
    }
    u64 el = net_ms() - genStart;
    note[0] = 0;
    if (state == 3) scat(note, u"返事を書いています  ", 120);
    if (el) { c16 b[16] = u""; u64 r10 = (u64)genTokens * 10000 / el; snum(b, (i64)(r10 / 10), 0, 16); scat(b, u".", 16); snum(b, (i64)(r10 % 10), 0, 16); scat(note, b, 120); scat(note, u" トークン/秒  ", 120); }
    scat(note, u"文脈 ", 120); snum(note, llm_pos(), 0, 120); scat(note, u"/", 120); snum(note, LLM_CTX, 0, 120);
    invalidate();
}
void chat_key(Key k) {
    if (k.scan == 0x0D) { kana_flush(in_add); kanaMode ^= 1; return; }   /* F3 */
    if (k.scan == 8) { llm_reset(); nmsg = 0; loglen = 0; if (state >= 2 && state != 9) { state = 2; msg_new(0); msg_addz(u"会話を忘れました。"); } return; } /* Delete */
    if (k.scan == 1) { scrollUp += 3; return; }
    if (k.scan == 2) { scrollUp = scrollUp > 3 ? scrollUp - 3 : 0; return; }
    if (state == 3) return; /* 返事中は入力しない */
    if (k.ch == 13) { send(); return; }
    if (k.ch == 8) { c16 r[8]; if (kana_pending(r)) { kana_flush(0); } if (ilen) ilen--; return; }
    if (k.ch >= 32) { if (kanaMode && k.ch < 127 && k.ch != u' ') kana_feed(k.ch, in_add); else { kana_flush(in_add); in_add(k.ch); } }
}
void chat_wheel(int dy) { scrollUp -= dy * 3; if (scrollUp < 0) scrollUp = 0; }

/* 吹き出しの中の行 (幅 w で折り返す): start..end を返す */
static int wrap(const c16 *s, int len, int w, int *starts, int max) {
    int n = 0, px = 0; starts[n++] = 0;
    for (int i = 0; i < len && n < max; i++) {
        if (s[i] == u'\n') { starts[n++] = i + 1; px = 0; continue; }
        int cw = charw(s[i]);
        if (px + cw > w) { starts[n++] = i; px = 0; }
        px += cw;
    }
    starts[n] = len;
    return n;
}
static int lastW = 560, lastH = 520;
void chat_draw(int x, int y, int w, int h) {
    lastW = w; lastH = h;
    fill(x, y, w, h, 0xF4F4FA);
    int inputH = 44, statusH = 22, areaH = h - inputH - statusH, bw = w * 3 / 4;
    /* 下から積む */
    static int starts[1200];
    int total = 0;
    for (int m = 0; m < nmsg; m++) total += wrap(logbuf + msg[m].start, msg[m].len, bw - 24, starts, 1100) * 18 + 22;
    int maxUp = total - areaH > 0 ? (total - areaH) / 18 + 1 : 0;
    if (scrollUp > maxUp) scrollUp = maxUp;
    int cy = y + areaH - total + scrollUp * 18 - 6;
    clip(x, y, w, areaH);
    for (int m = 0; m < nmsg; m++) {
        int nl = wrap(logbuf + msg[m].start, msg[m].len, bw - 24, starts, 1100);
        int tw = 0;
        if (nl == 1) { for (int i = 0; i < msg[m].len; i++) tw += charw(logbuf[msg[m].start + i]); } else tw = bw - 24;
        int bh = nl * 18 + 12, bwid = tw + 24 < 60 ? 60 : tw + 24;
        int bx = msg[m].user ? x + w - bwid - 12 : x + 44;
        if (cy + bh > y && cy < y + areaH) {
            if (!msg[m].user) { /* AI の印 */
                fill(x + 12, cy + 2, 24, 24, 0x1E1C34);
                text(x + 16, cy + 6, u"∞", 0xEC78DC);
            }
            fill(bx + 2, cy + 2, bwid, bh, 0xDDDDEA);
            fill(bx, cy, bwid, bh, msg[m].user ? 0x6D5DFC : 0xFFFFFF);
            for (int l = 0; l < nl; l++) {
                int a = starts[l], b = starts[l + 1], px = bx + 12;
                for (int i = a; i < b; i++) { c16 c = logbuf[msg[m].start + i]; if (c == u'\n') continue; px += glyph(px, cy + 6 + l * 18, c, msg[m].user ? 0xFFFFFF : 0x1D1D27); }
            }
            if (m == nmsg - 1 && state == 3 && (net_ms() / 400) % 2) fill(bx + 12 + (nl == 1 ? tw : 0), cy + 6 + (nl - 1) * 18 + 2, 8, 14, 0x6D5DFC);
        }
        cy += bh + 10;
    }
    noclip();
    /* 入力欄 */
    int iy = y + areaH;
    fill(x, iy, w, inputH, 0xFFFFFF); fill(x, iy, w, 1, 0xDDDDEA);
    fill(x + 10, iy + 8, w - 100, 28, state == 2 ? 0x6D5DFC : 0xC8C8D8); fill(x + 11, iy + 9, w - 102, 26, 0xFFFFFF);
    clip(x + 14, iy + 8, w - 108, 28);
    int px = x + 18, tw = 0; for (int i = 0; i < ilen; i++) tw += charw(input[i]);
    if (tw > w - 130) px -= tw - (w - 130);
    for (int i = 0; i < ilen; i++) px += glyph(px, iy + 14, input[i], 0x1D1D27);
    c16 r[8]; if (kana_pending(r)) { int rw = 0; for (int i = 0; r[i]; i++) rw += charw(r[i]); fill(px, iy + 13, rw, 18, 0xFFE9A8); text(px, iy + 14, r, 0x6B4E00); px += rw; }
    if (state == 2) fill(px + 1, iy + 13, 2, 18, 0x6D5DFC);
    if (!ilen && state == 2 && !r[0]) text(x + 22, iy + 14, kanaMode ? u"メッセージ (かな入力)" : u"メッセージ (F3 でかな入力)", 0xA0A0B8);
    noclip();
    fill(x + w - 82, iy + 8, 72, 28, state == 2 ? 0x6D5DFC : 0xB8B8CC);
    text(x + w - 62, iy + 14, u"送信", 0xFFFFFF);
    /* 状態 */
    fill(x, y + h - statusH, w, statusH, 0xE4E4F2);
    c16 s[160] = u""; scat(s, kanaMode ? u"[あ] " : u"[A] ", 160); scat(s, note, 160);
    text(x + 8, y + h - 19, s, 0x55556A);
}
void chat_click(int x, int y) { /* 送信ボタン */
    if (x >= lastW - 82 && x < lastW - 10 && y >= lastH - 22 - 44 + 8 && y < lastH - 22 - 8) send();
}
