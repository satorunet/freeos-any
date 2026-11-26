/* 起動アニメーション: ロゴが現れ、窓が開き、F と A が書かれ、光が走り、名前が浮かぶ。
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
static float d_seg(float px, float py, float ax, float ay, float bx, float by) {
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float l = vx * vx + vy * vy, t = l > 0 ? clamp01((wx * vx + wy * vy) / l) : 0;
    float dx = wx - vx * t, dy = wy - vy * t;
    return fsqrt(dx * dx + dy * dy);
}

/* F と A の線 (アイコンと同じ形)。書き順どおりに並べる */
typedef struct { float ax, ay, bx, by, w; } Stroke;
static const Stroke strokes[] = {
    {160, 350, 160, 222, 28}, {160, 222, 232, 222, 28}, {160, 280, 220, 280, 26},
    {270, 356, 316, 218, 28}, {316, 218, 362, 356, 28}, {290, 304, 342, 304, 24},
};
#define NSTROKE 6

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
static int big_width(const c16 *s) { int w = 0; for (; *s; s++) w += big_adv(*s) + 1; return w - 1; }

/* 1コマ描く。ms は開始からの時間 */
static void frame(u32 *bg, float ms) {
    mcopy(scr, bg, (u64)SW * SH * 4);
    int size = SH < 600 ? 150 : 200;
    float cx = SW / 2.0f, cy = SH / 2.0f - size * 0.35f;

    float pop = span(ms, 0, 650), sc = 0.35f + 0.65f * ease_back(pop), alpha = ease_out(pop * 1.6f);
    float k = size / 440.0f * sc;                 /* 1単位が何画面ピクセルか */
    float open = ease_out(span(ms, 380, 820));     /* 窓が開く */
    float ink = span(ms, 760, 1380);               /* 線を書く */
    float shine = span(ms, 1420, 2000);            /* 光が走る */

    /* 後ろの光 */
    int glowR = (int)(size * 1.1f);
    for (int y = (int)cy - glowR; y < cy + glowR; y++) for (int x = (int)cx - glowR; x < cx + glowR; x++) {
        float dx = x - cx, dy = y - cy, d = fsqrt(dx * dx + dy * dy) / glowR;
        blend(x, y, 0x7D6DFF, (1 - smooth(0, 1, d)) * 0.28f * alpha);
    }

    int half = (int)(size * 0.62f * sc) + 30;
    float aa = 1 / k; /* 1ピクセル分の幅 (設計図の単位) */
    float ink_len = 0, total = 0;
    for (int i = 0; i < NSTROKE; i++) { float dx = strokes[i].bx - strokes[i].ax, dy = strokes[i].by - strokes[i].ay; total += fsqrt(dx * dx + dy * dy); }
    ink_len = ink * total;
    float wy1 = 186 + (382 - 186) * open; /* 窓の下端 */

    for (int y = (int)cy - half; y < cy + half; y++) for (int x = (int)cx - half; x < cx + half; x++) {
        float px = (x + 0.5f - cx) / k + 256, py = (y + 0.5f - cy) / k + 256;
        /* 影 */
        float ds = d_rrect(px, py - 14, 36, 36, 476, 476, 100);
        blend(x, y, 0x05050C, (1 - smooth(-10, 40, ds)) * 0.45f * alpha);
        /* 紫の角丸 (上から下へ少し濃く) */
        float d = d_rrect(px, py, 36, 36, 476, 476, 100), cov = clamp01(0.5f - d / aa);
        if (cov <= 0) continue;
        u32 base = mix(0x8576FF, 0x5B49EE, clamp01((py - 36) / 440));
        blend(x, y, base, cov * alpha);
        /* 窓 */
        if (open > 0) {
            float wa = clamp01(open * 3);
            float dw = d_rrect(px, py, 100, 130, 412, wy1, 24);
            blend(x, y, 0xF4F4FA, clamp01(0.5f - dw / aa) * wa * alpha);
            float dt = d_rrect(px, py, 100, 130, 412, 186, 24);
            float bar = clamp01(0.5f - dt / aa) * (py < 186 ? 1 : 0);
            if (py >= 162 && py < 186) bar = clamp01(0.5f - dw / aa); /* 下側は角を丸めない */
            blend(x, y, 0x242440, bar * wa * alpha);
            /* タイトルバーの丸ボタン */
            for (int b = 0; b < 3; b++) {
                float bx = 130 + b * 26, by = 158, db = fsqrt((px - bx) * (px - bx) + (py - by) * (py - by)) - 8;
                static const u32 bc[3] = {0xFF6B81, 0xFFC857, 0x4ADE80};
                blend(x, y, bc[b], clamp01(0.5f - db / aa) * wa * alpha * span(ms, 650 + b * 60, 800 + b * 60));
            }
        }
        /* F と A を書き順どおりに */
        if (ink > 0) {
            float left = ink_len, best = 1e9f;
            for (int i = 0; i < NSTROKE && left > 0; i++) {
                const Stroke *s = &strokes[i];
                float dx = s->bx - s->ax, dy = s->by - s->ay, l = fsqrt(dx * dx + dy * dy), f = left >= l ? 1 : left / l;
                float dd = d_seg(px, py, s->ax, s->ay, s->ax + dx * f, s->ay + dy * f) - s->w / 2;
                if (dd < best) best = dd;
                left -= l;
            }
            blend(x, y, 0x6D5DFC, clamp01(0.5f - best / aa) * alpha);
        }
        /* 光の帯 (斜め) */
        if (shine > 0 && shine < 1) {
            float pos = -200 + shine * 900, band = fabsf_((px + py * 0.6f) - pos);
            blend(x, y, 0xFFFFFF, (1 - smooth(0, 60, band)) * 0.35f * cov);
        }
    }

    /* 名前: 1文字ずつ下から浮かぶ */
    int tw = big_width(TITLE), tx = (SW - tw) / 2, ty = (int)(cy + size * 0.62f + 28);
    for (int i = 0; TITLE[i]; i++) {
        float t = ease_out(span(ms, 1150 + i * 45, 1500 + i * 45));
        if (t > 0) big_glyph(tx, ty + (int)((1 - t) * 14), TITLE[i], 0xFFFFFF, t);
        tx += big_adv(TITLE[i]) + 1;
    }
    /* 版 */
    float va = ease_out(span(ms, 1700, 2100));
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

#define LENGTH 2500   /* アニメーションの長さ (ミリ秒) */
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
