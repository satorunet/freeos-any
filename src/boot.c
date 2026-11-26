/* 起動アニメーション: 角丸が現れ、虹色の ∞ が一筆で描かれ、Any が浮かび、光が走り、名前が出る。
 * キーを押すと飛ばせる。最後はデスクトップへ溶けるように切り替わる。 */
#include "os.h"
#include "net.h"

static float fsqrt(float x) {
    if (x <= 0) return 0;
#if defined(__aarch64__)
    float r; __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x)); return r;
#else
    float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r;
#endif
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float fmaxf_(float a, float b) { return a > b ? a : b; }
static float fminf_(float a, float b) { return a < b ? a : b; }
static float fabsf_(float a) { return a < 0 ? -a : a; }
static float smooth(float a, float b, float x) { float t = clamp01((x - a) / (b - a)); return t * t * (3 - 2 * t); }
static float ease_out(float t) { t = clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }
static float ease_back(float t) { t = clamp01(t); float c = 1.70158f * 1.2f, u = t - 1; return 1 + (c + 1) * u * u * u + c * u * u; }
/* 区間 [a,b] (ミリ秒) での進み具合 0..1 */
static float span(float ms, float a, float b) { return clamp01((ms - a) / (b - a)); }

static u32 *scr;
static void blend(int x, int y, u32 c, float a) {
    if (a <= 0 || x < 0 || y < 0 || x >= SW || y >= SH) return;
    if (a > 1) a = 1;
    u32 *p = &scr[y * SW + x], d = *p;
    u32 r = (u32)(((c >> 16) & 255) * a + ((d >> 16) & 255) * (1 - a));
    u32 g = (u32)(((c >> 8) & 255) * a + ((d >> 8) & 255) * (1 - a));
    u32 b = (u32)((c & 255) * a + (d & 255) * (1 - a));
    *p = r << 16 | g << 8 | b;
}
static u32 mix(u32 a, u32 b, float t) {
    u32 r = (u32)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
    u32 g = (u32)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
    u32 bl = (u32)((a & 255) * (1 - t) + (b & 255) * t);
    return r << 16 | g << 8 | bl;
}

/* 形の距離 (負なら内側)。座標はアイコンの設計図 (512 x 512) の単位 */
static float d_rrect(float px, float py, float x0, float y0, float x1, float y1, float r) {
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
    float qx = fabsf_(px - cx) - hx, qy = fabsf_(py - cy) - hy;
    float ox = fmaxf_(qx, 0), oy = fmaxf_(qy, 0);
    return fsqrt(ox * ox + oy * oy) + fminf_(fmaxf_(qx, qy), 0) - r;
}

/* ∞ (レムニスケート)。設計図 512 x 512 の単位。形と色は tools/logo.py と同じ */
#define LCX 256.0f
#define LCY 214.0f
#define LA 168.0f
#define LW 44.0f
static float fsin(float x) { /* -π..π に寄せてから多項式 */
    const float PI = 3.14159265f;
    while (x > PI) x -= 2 * PI;
    while (x < -PI) x += 2 * PI;
    float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}
static float fcos(float x) { return fsin(x + 1.5707963f); }
static void lem(float t, float *x, float *y) {
    float sn = fsin(t), cs = fcos(t), d = 1 + sn * sn;
    *x = LCX + LA * cs / d; *y = LCY + LA * sn * cs / d;
}
static u32 rainbow(float t) { /* t は 0..1 */
    static const u32 c[5] = {0x9682FF, 0x6D5DFC, 0xEC78DC, 0xFFBE5A, 0x9682FF};
    t = clamp01(t) * 4; int i = (int)t; if (i > 3) i = 3;
    return mix(c[i], c[i + 1], t - i);
}
/* 線を描きためる板 (アイコンの範囲だけ) */
#define SB 320
static float cover[SB * SB];
static u32 scol[SB * SB];
static float drawnT; /* どこまで描いたか */
static int sbx, sby;  /* 板の左上の画面座標 */
static void stamp(float t, float k, float cx, float cy) {
    float ux, uy; lem(t, &ux, &uy);
    float px = (ux - 256) * k + cx - sbx, py = (uy - 256) * k + cy - sby, r = LW / 2 * k;
    u32 col = rainbow(t / 6.2831853f);
    for (int y = (int)(py - r - 1); y <= (int)(py + r + 1); y++) for (int x = (int)(px - r - 1); x <= (int)(px + r + 1); x++) {
        if (x < 0 || y < 0 || x >= SB || y >= SB) continue;
        float dx = x + 0.5f - px, dy = y + 0.5f - py, c = clamp01(r - fsqrt(dx * dx + dy * dy) + 0.5f);
        if (c <= 0) continue;
        int i = y * SB + x;
        scol[i] = col;
        if (c > cover[i]) cover[i] = c;
    }
}
/* 交差点で手前を通る線の下に影を落とす */
static void cross_shadow(float k, float cx, float cy) {
    for (int y = 0; y < SB; y++) for (int x = 0; x < SB; x++) {
        int i = y * SB + x; if (cover[i] <= 0) continue;
        float best = 1e9f;
        for (int j = 0; j <= 20; j++) {
            float ux, uy; lem(4.712389f - 0.25f + 0.5f * j / 20, &ux, &uy);
            float dx = (ux - 256) * k + cx - sbx - x, dy = (uy - 256) * k + cy - sby - y, d = fsqrt(dx * dx + dy * dy);
            if (d < best) best = d;
        }
        float a = (1 - smooth(LW / 2 * k, (LW / 2 + 14) * k, best)) * 0.45f;
        if (a > 0) scol[i] = mix(scol[i], 0x000000, a);
    }
}

static const c16 *TITLE = u"FreeOS Any";

/* 背景: 中央が少し明るい夜空のような紫 */
static void background(u32 *buf) {
    float cx = SW / 2.0f, cy = SH / 2.0f - 30, rmax = fsqrt(cx * cx + cy * cy);
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) {
        float dx = x - cx, dy = y - cy, d = fsqrt(dx * dx + dy * dy) / rmax;
        buf[y * SW + x] = mix(0x2B2558, 0x0E0E1C, smooth(0, 1, d));
    }
}

/* 名前の大きな文字 (BIZ UDPゴシック Bold) */
static int big_glyph(int x, int y, c16 ch, u32 color, float alpha) {
    int w, adv, h; const u8 *b = title_bitmap(ch, &w, &adv, &h);
    if (!b) return 12;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) if (b[j * w + i]) blend(x + i - 1, y + j, color, b[j * w + i] / 255.0f * alpha);
    return adv;
}
static int big_adv(c16 ch) { int w, adv, h; return title_bitmap(ch, &w, &adv, &h) ? adv : 12; }
#define TITLE_HALF 17
static int big_width(const c16 *s) { int w = 0; for (; *s; s++) w += big_adv(*s) + 1; return w - 1; }

/* 1コマ描く。ms は開始からの時間 */
static void frame(u32 *bg, float ms) {
    mcopy(scr, bg, (u64)SW * SH * 4);
    int size = SH < 600 ? 150 : 200;
    float cx = SW / 2.0f, cy = SH / 2.0f - size * 0.35f;

    float pop = span(ms, 0, 600), sc = 0.35f + 0.65f * ease_back(pop), alpha = ease_out(pop * 1.6f);
    float k = size / 440.0f * sc;                  /* 1単位が何画面ピクセルか */
    float draw = span(ms, 520, 1500);              /* ∞ を描く */
    float shine = span(ms, 1650, 2200);            /* 光が走る */
    float aa = 1 / k;

    /* 後ろの光 */
    int glowR = (int)(size * 1.1f);
    for (int y = (int)cy - glowR; y < cy + glowR; y++) for (int x = (int)cx - glowR; x < cx + glowR; x++) {
        float dx = x - cx, dy = y - cy, d = fsqrt(dx * dx + dy * dy) / glowR;
        blend(x, y, 0x7D6DFF, (1 - smooth(0, 1, d)) * 0.22f * alpha);
    }
    /* 角丸 (夜空色) と影 */
    int half = (int)(size * 0.62f * sc) + 30;
    for (int y = (int)cy - half; y < cy + half; y++) for (int x = (int)cx - half; x < cx + half; x++) {
        float px = (x + 0.5f - cx) / k + 256, py = (y + 0.5f - cy) / k + 256;
        float ds = d_rrect(px, py - 14, 24, 24, 488, 488, 110);
        blend(x, y, 0x05050C, (1 - smooth(-10, 40, ds)) * 0.5f * alpha);
        float d = d_rrect(px, py, 24, 24, 488, 488, 110), cov = clamp01(0.5f - d / aa);
        if (cov > 0) blend(x, y, mix(0x1E1C34, 0x100F1C, clamp01((py - 24) / 464)), cov * alpha);
    }
    /* ∞: 右端から一筆で。大きさが決まってから描き始め、描いた分を板に貯める */
    if (draw > 0) {
        float e = draw < 0.5f ? 2 * draw * draw : 1 - 2 * (1 - draw) * (1 - draw); /* ゆっくり→速く→ゆっくり */
        float target = e * 6.2831853f, step = 0.004f;
        if (drawnT == 0) { memset(cover, 0, sizeof cover); sbx = (int)cx - SB / 2; sby = (int)cy - SB / 2; }
        while (drawnT < target) {
            if (drawnT < 4.212389f && drawnT + step >= 4.212389f) cross_shadow(k, cx, cy); /* 交差点の手前で影 */
            stamp(drawnT, k, cx, cy);
            drawnT += step;
        }
        for (int y = 0; y < SB; y++) for (int x = 0; x < SB; x++) {
            int i = y * SB + x;
            if (cover[i] > 0) blend(sbx + x, sby + y, scol[i], cover[i]);
        }
    }
    /* 角丸の中の Any */
    float ta = ease_out(span(ms, 1300, 1750));
    if (ta > 0) {
        int tw = big_width(u"Any"), x = (int)cx - tw / 2, y = (int)(cy + (392 - 256) * k) - TITLE_HALF + (int)((1 - ta) * 10);
        for (const c16 *p = u"Any"; *p; p++) x += big_glyph(x, y, *p, 0xFFFFFF, ta) + 1;
    }
    /* 光の帯 (斜め、角丸の中だけ) */
    if (shine > 0 && shine < 1) {
        for (int y = (int)cy - half; y < cy + half; y++) for (int x = (int)cx - half; x < cx + half; x++) {
            float px = (x + 0.5f - cx) / k + 256, py = (y + 0.5f - cy) / k + 256;
            float cov = clamp01(0.5f - d_rrect(px, py, 24, 24, 488, 488, 110) / aa);
            if (cov <= 0) continue;
            float pos = -200 + shine * 1000, band = fabsf_((px + py * 0.6f) - pos);
            blend(x, y, 0xFFFFFF, (1 - smooth(0, 60, band)) * 0.16f * cov);
        }
    }

    /* 名前: 1文字ずつ下から浮かぶ */
    int tw = big_width(TITLE), tx = (SW - tw) / 2, ty = (int)(cy + size * 0.62f + 36);
    for (int i = 0; TITLE[i]; i++) {
        float t = ease_out(span(ms, 1500 + i * 40, 1850 + i * 40));
        if (t > 0) big_glyph(tx, ty + (int)((1 - t) * 14), TITLE[i], 0xFFFFFF, t);
        tx += big_adv(TITLE[i]) + 1;
    }
    /* 版 */
    float va = ease_out(span(ms, 1950, 2300));
    if (va > 0) {
        const c16 *v = u"version " FREEOS_VER; int vw = 0;
        for (const c16 *p = v; *p; p++) vw += charw(*p);
        int x = (SW - vw) / 2;
        u32 col = mix(0x0E0E1C, 0xB8B8E0, va);
        text(x, ty + 46, v, col);
    }
}

static int key_pressed(void) {
    Key k; int any = 0;
    while (!ST->conIn->ReadKeyStroke(ST->conIn, &k)) any = 1;
    return any;
}

#define LENGTH 2700   /* アニメーションの長さ (ミリ秒) */
#define FADE 450      /* デスクトップへ切り替わる時間 */

void boot_splash(void) {
    u32 *bg, *logo, *desk;
    u64 bytes = (u64)SW * SH * 4;
    if (BS->AllocatePool(2, bytes, (void **)&bg)) return;
    if (BS->AllocatePool(2, bytes, (void **)&logo)) { BS->FreePool(bg); return; }
    if (BS->AllocatePool(2, bytes, (void **)&desk)) { BS->FreePool(bg); BS->FreePool(logo); return; }
    scr = screen();
    background(bg);
    sound_chime();
    u64 t0 = net_ms();
    for (;;) {
        float ms = (float)(net_ms() - t0);
        if (ms >= LENGTH || key_pressed()) break;
        frame(bg, ms);
        screen_show();
        sound_poll();
    }
    frame(bg, LENGTH);
    mcopy(logo, scr, bytes);
    /* デスクトップを作ってから重ねて切り替える */
    desktop_compose();
    mcopy(desk, scr, bytes);
    u64 t1 = net_ms();
    for (;;) {
        float t = (float)(net_ms() - t1) / FADE;
        if (t >= 1) break;
        float e = t * t * (3 - 2 * t);
        for (u64 i = 0; i < (u64)SW * SH; i++) scr[i] = mix(logo[i], desk[i], e);
        screen_show();
        sound_poll();
    }
    mcopy(scr, desk, bytes);
    screen_show();
    BS->FreePool(bg); BS->FreePool(logo); BS->FreePool(desk);
}
