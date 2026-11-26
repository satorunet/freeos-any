/* ソリティア (クロンダイク、1枚めくり) */
#include "os.h"

/* 列: 0=山札 1=めくり札 2-5=組札 6-12=場札 */
#define PILES 13
typedef struct { u8 n[PILES], down[PILES], c[PILES][24 + 1]; int moves; } Game;
static Game g, undo[128];
static int nundo, cur = 6, depth = 1, held = -1, heldCount;
static u32 seed;

static int rank(u8 c) { return c % 13; }
static int suit(u8 c) { return c / 13; }       /* 0♠ 1♥ 2♦ 3♣ */
static int red(u8 c) { return suit(c) == 1 || suit(c) == 2; }
static void save(void) { if (nundo == 128) mcopy(undo, undo + 1, sizeof undo - sizeof *undo), nundo--; undo[nundo++] = g; }

static void deal(void) {
    Time t; if (!ST->rt->GetTime(&t, 0)) seed ^= t.second * 7919 + t.minute * 104729 + t.hour * 31 + t.day * 1009 + t.ns;
    u8 d[52];
    for (int i = 0; i < 52; i++) d[i] = i;
    for (int i = 51; i > 0; i--) { seed = seed * 1103515245 + 12345; int j = (seed >> 16) % (i + 1); u8 v = d[i]; d[i] = d[j]; d[j] = v; }
    memset(&g, 0, sizeof g);
    int k = 0;
    for (int i = 0; i < 7; i++) { for (int j = 0; j <= i; j++) g.c[6 + i][g.n[6 + i]++] = d[k++]; g.down[6 + i] = i; }
    while (k < 52) g.c[0][g.n[0]++] = d[k++];
    nundo = 0; held = -1; cur = 0; depth = 1;
}
static void draw_stock(void) {
    if (!g.n[0] && !g.n[1]) return;
    save();
    if (g.n[0]) g.c[1][g.n[1]++] = g.c[0][--g.n[0]];
    else while (g.n[1]) g.c[0][g.n[0]++] = g.c[1][--g.n[1]];
    g.moves++;
}
static int can_drop(int from, int count, int to) {
    if (from == to || to < 2 || count < 1 || count > g.n[from]) return 0;
    u8 c = g.c[from][g.n[from] - count];
    if (to < 6) return count == 1 && (g.n[to] ? suit(c) == suit(g.c[to][g.n[to] - 1]) && rank(c) == rank(g.c[to][g.n[to] - 1]) + 1 : rank(c) == 0);
    if (!g.n[to]) return rank(c) == 12;
    u8 t = g.c[to][g.n[to] - 1];
    return red(c) != red(t) && rank(c) == rank(t) - 1;
}
static void move(int from, int count, int to) {
    save();
    for (int i = 0; i < count; i++) g.c[to][g.n[to] + i] = g.c[from][g.n[from] - count + i];
    g.n[to] += count; g.n[from] -= count; g.moves++;
    if (from >= 6 && g.n[from] && g.down[from] >= g.n[from]) g.down[from] = g.n[from] - 1; /* 裏の札をめくる */
}
static int to_foundation(int from) {
    if (from == 0 || (from >= 2 && from < 6) || !g.n[from]) return 0;
    for (int to = 2; to < 6; to++) if (can_drop(from, 1, to)) { move(from, 1, to); return 1; }
    return 0;
}
static int won(void) { return g.n[2] + g.n[3] + g.n[4] + g.n[5] == 52; }
static int movable(int p) { return p >= 6 ? g.n[p] - g.down[p] : g.n[p] ? 1 : 0; }
static void activate(void) {
    if (cur == 0) { held = -1; draw_stock(); return; }
    if (held < 0) {
        if (!g.n[cur]) return;
        held = cur; heldCount = cur >= 6 ? depth : 1;
    } else if (held == cur) { /* もう一度押すと組札へ */
        if (heldCount == 1) to_foundation(cur);
        held = -1;
    } else {
        int n = heldCount;
        if (cur >= 6 && held >= 6) /* 置ける枚数を自動で選ぶ */
            for (int k = 1; k <= movable(held); k++) if (can_drop(held, k, cur)) { n = k; if (k == heldCount) break; }
        if (can_drop(held, n, cur)) { move(held, n, cur); held = -1; depth = 1; }
        else if (g.n[cur]) { held = cur; heldCount = cur >= 6 ? depth : 1; }
        else held = -1;
    }
}
void solitaire_open(void) { deal(); }
void solitaire_key(Key k) {
    if (k.scan == 4) cur = cur == 0 ? 12 : cur - 1, depth = 1;
    else if (k.scan == 3) cur = (cur + 1) % PILES, depth = 1;
    else if (k.scan == 1) { if (cur >= 6 && depth < movable(cur)) depth++; else if (cur >= 6) cur = cur - 6 < 2 ? cur - 6 : cur - 7 < 2 ? 2 : cur - 7, depth = 1; }
    else if (k.scan == 2) { if (cur >= 6 && depth > 1) depth--; else if (cur < 6) cur = cur < 2 ? cur + 6 : cur + 7, depth = 1; }
    else if (k.ch == 13) activate();
    else if (k.ch == u' ') held = -1, draw_stock();
    else if (k.ch == u'a' || k.ch == u'A') { held = -1; int again = 1; while (again) { again = to_foundation(1); for (int p = 6; p < PILES; p++) again |= to_foundation(p); } }
    else if ((k.ch == u'u' || k.ch == u'U') && nundo) g = undo[--nundo], held = -1, depth = 1;
    else if (k.ch == u'n' || k.ch == u'N') deal();
    if (depth > movable(cur)) depth = 1;
}

#define CW 44
#define CH 58
static int px(int p) { return 10 + (p < 2 ? p : p < 6 ? p + 1 : p - 6) * 54; }
static int py(int p) { return p < 6 ? 28 : 96; }
static int card_y(int p, int i) { return py(p) + (i < g.down[p] ? i * 6 : g.down[p] * 6 + (i - g.down[p]) * 17); }
static void card(int x, int y, int c, int mark) {
    static const c16 *ranks[13] = {u"A", u"2", u"3", u"4", u"5", u"6", u"7", u"8", u"9", u"10", u"J", u"Q", u"K"};
    static const c16 suits[4] = {u'♠', u'♥', u'♦', u'♣'};
    fill(x, y, CW, CH, mark == 2 ? 0xFFB020 : mark == 1 ? 0x6D5DFC : 0x333344);
    int b = mark ? 3 : 1;
    fill(x + b, y + b, CW - 2 * b, CH - 2 * b, c < 0 ? 0x3355AA : 0xFFFFFF);
    if (c < 0) return;
    u32 col = red(c) ? 0xD01030 : 0x101018;
    glyph(text(x + 5, y + 2, ranks[rank(c)], col), y + 2, suits[suit(c)], col);
    glyph(x + 18, y + 30, suits[suit(c)], col);
}
void solitaire_draw(int x, int y, int w, int h) {
    fill(x, y, w, h, 0x146B3A);
    c16 s[64] = u"手数 "; snum(s, g.moves, 0, 64);
    scat(s, won() ? u"   クリア！ N で次のゲーム" : u"   Space:めくる A:自動 U:戻す N:新規", 64);
    text(x + 10, y + 6, s, 0xFFFFFF);
    for (int p = 0; p < PILES; p++) {
        int cx = x + px(p), cy = y + py(p), at = p == cur, hd = p == held;
        if (!g.n[p]) { fill(cx, cy, CW, CH, at ? 0x6D5DFC : 0x2E8B57); fill(cx + 3, cy + 3, CW - 6, CH - 6, 0x146B3A); if (p == 0) text(cx + 14, cy + 20, u"○", 0xBBDDCC); continue; }
        if (p < 6) { card(cx, cy, p == 0 ? -1 : g.c[p][g.n[p] - 1], hd ? 2 : at ? 1 : 0); continue; }
        for (int i = 0; i < g.n[p]; i++) {
            int mark = hd && i >= g.n[p] - heldCount ? 2 : at && i >= g.n[p] - depth ? 1 : 0;
            card(cx, y + card_y(p, i), i < g.down[p] ? -1 : g.c[p][i], mark);
        }
    }
}
void solitaire_click(int x, int y) {
    for (int p = 0; p < PILES; p++) {
        if (x < px(p) || x >= px(p) + CW || y < py(p)) continue;
        if (p < 6) { if (y >= py(p) + CH) continue; cur = p; depth = 1; activate(); return; }
        int hit = g.n[p] ? -1 : 0;
        for (int i = g.n[p] - 1; i >= g.down[p] && hit < 0; i--) if (y >= card_y(p, i) && y < card_y(p, i) + (i == g.n[p] - 1 ? CH : 17)) hit = i;
        if (hit < 0) return;
        cur = p; depth = g.n[p] ? g.n[p] - hit : 1; activate();
        return;
    }
}
