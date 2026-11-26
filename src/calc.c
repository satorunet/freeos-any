/* 電卓: かけ算・割り算を先に計算する (12 + 3 × 2 = 18)。マウスでもキーボードでも使える。
 * キー: 数字 . + - * / % = Enter BackSpace、Delete か c で全部消す */
#include "os.h"

#define MAXTOK 32
#define DIGITS 15
static double num[MAXTOK];
static c16 op[MAXTOK];   /* num[i] の後ろの演算子 */
static int ntok;         /* 確定した数の個数 */
static c16 entry[DIGITS + 3]; /* 入力中の数 ("" なら未入力) */
static double result;
static int shown, error; /* = の直後か / エラーか */

static double parse(const c16 *s) {
    double v = 0, scale = 0; int neg = 0;
    if (*s == u'-') neg = 1, s++;
    for (; *s; s++) {
        if (*s == u'.') { scale = 1; continue; }
        v = v * 10 + (*s - u'0');
        if (scale) scale *= 10;
    }
    if (scale) v /= scale;
    return neg ? -v : v;
}

/* 数を文字にする (有効数字 sig 桁、余分な0は省く。大きすぎ・小さすぎは指数表記) */
static int sig = 12;
static void format(double v, c16 *out, int cap) {
    out[0] = 0;
    if (v != v || v > 1e300 || v < -1e300) { scopy(out, u"エラー", cap); return; }
    if (v < 0) { scat(out, u"-", cap); v = -v; }
    int exp10 = 0;
    if (v != 0 && (v >= 1e12 || v < 1e-9)) {
        while (v >= 10) v /= 10, exp10++;
        while (v < 1) v *= 10, exp10--;
    }
    /* 整数部と小数部を分けて12桁に丸める */
    int intDigits = 1; for (double t = v; t >= 10; t /= 10) intDigits++;
    int frac = sig - intDigits; if (frac < 0) frac = 0;
    double p = 1; for (int i = 0; i < frac; i++) p *= 10;
    u64 whole = (u64)v, f = (u64)((v - (double)whole) * p + 0.5);
    if (f >= (u64)p) whole++, f -= (u64)p;
    if (exp10 && whole >= 10) { whole /= 10; exp10++; } /* 9.99… が 10 に繰り上がった */
    c16 w[24] = u""; snum(w, (i64)whole, 0, 24);
    /* 3桁ごとにカンマ */
    int n = slen(w), o = slen(out);
    for (int i = 0; i < n && o < cap - 2; i++) {
        out[o++] = w[i];
        if ((n - 1 - i) % 3 == 0 && i != n - 1 && !exp10) out[o++] = u',';
    }
    out[o] = 0;
    if (frac && f) {
        c16 fs[24] = u""; snum(fs, (i64)f, frac, 24);
        int k = slen(fs); while (k && fs[k - 1] == u'0') fs[--k] = 0;
        scat(out, u".", cap); scat(out, fs, cap);
    }
    if (exp10) { scat(out, u"e", cap); snum(out, exp10, 0, cap); }
}

static int prec(c16 o) { return o == u'×' || o == u'÷' ? 2 : 1; }
static double apply(double a, c16 o, double b) {
    if (o == u'+') return a + b;
    if (o == u'−') return a - b;
    if (o == u'×') return a * b;
    if (b == 0) { error = 1; return 0; }
    return a / b;
}
/* かけ算・割り算を先に計算する */
static double evaluate(const double *v, const c16 *o, int n) {
    double vals[MAXTOK]; c16 ops[MAXTOK]; int nv = 0, no = 0;
    for (int i = 0; i < n; i++) {
        vals[nv++] = v[i];
        if (i == n - 1) break;
        while (no && prec(ops[no - 1]) >= prec(o[i])) { nv--; vals[nv - 1] = apply(vals[nv - 1], ops[--no], vals[nv]); }
        ops[no++] = o[i];
    }
    while (no) { nv--; vals[nv - 1] = apply(vals[nv - 1], ops[--no], vals[nv]); }
    return vals[0];
}
/* 今の式の値 (入力中の数まで含める) */
static double preview(void) {
    double v[MAXTOK]; c16 o[MAXTOK]; int n = ntok;
    for (int i = 0; i < n; i++) v[i] = num[i], o[i] = op[i];
    if (entry[0]) v[n++] = parse(entry);
    if (!n) return 0;
    return evaluate(v, o, n);
}

static void clear_all(void) { ntok = 0; entry[0] = 0; result = 0; shown = error = 0; }
static void digit(c16 c) {
    if (shown || error) clear_all();
    int n = slen(entry), digits = 0;
    for (int i = 0; i < n; i++) if (entry[i] >= u'0' && entry[i] <= u'9') digits++;
    if (c == u'.') {
        for (int i = 0; i < n; i++) if (entry[i] == u'.') return;
        if (!n || (n == 1 && entry[0] == u'-')) scat(entry, u"0", DIGITS + 3);
    } else {
        if (digits >= DIGITS) return;
        if (n == 1 && entry[0] == u'0') n = 0, entry[0] = 0;              /* 先頭の 0 を置き換える */
        if (n == 2 && entry[0] == u'-' && entry[1] == u'0') n = 1, entry[1] = 0;
    }
    c16 s[2] = {c, 0};
    scat(entry, s, DIGITS + 3);
}
static void oper(c16 o) {
    if (error) clear_all();
    if (shown) { /* 答えの続きから計算する */
        ntok = 0; format(result, entry, DIGITS + 3);
        c16 t[DIGITS + 3]; int k = 0; for (int i = 0; entry[i]; i++) if (entry[i] != u',') t[k++] = entry[i];
        t[k] = 0; scopy(entry, t, DIGITS + 3);
        shown = 0;
        if (entry[0] == u'エ' || entry[slen(entry) - 1] == u'e') { num[0] = result; ntok = 1; op[0] = o; entry[0] = 0; return; }
    }
    if (!entry[0]) {
        if (ntok) op[ntok - 1] = o; /* 演算子の入れ替え */
        else if (o == u'−') scopy(entry, u"-", DIGITS + 3);
        return;
    }
    if (ntok >= MAXTOK - 1) return;
    num[ntok] = parse(entry); op[ntok] = o; ntok++;
    entry[0] = 0;
}
static void equals(void) {
    if (shown || error) return;
    if (!entry[0] && !ntok) return;
    result = preview();
    shown = 1;
}
static void back_space(void) {
    if (shown || error) { clear_all(); return; }
    int n = slen(entry);
    if (n) entry[n - 1] = 0;
    else if (ntok) { /* 演算子を消して、前の数を入力中に戻す */
        ntok--; format(num[ntok], entry, DIGITS + 3);
        c16 t[DIGITS + 3]; int k = 0; for (int i = 0; entry[i]; i++) if (entry[i] != u',') t[k++] = entry[i];
        t[k] = 0; scopy(entry, t, DIGITS + 3);
    }
}
static void negate(void) {
    if (shown) { result = -result; return; }
    if (!entry[0]) { scopy(entry, u"-", DIGITS + 3); return; }
    if (entry[0] == u'-') scopy(entry, entry + 1, DIGITS + 3);
    else { c16 t[DIGITS + 3] = u"-"; scat(t, entry, DIGITS + 3); scopy(entry, t, DIGITS + 3); }
}
static void percent(void) {
    if (shown) { result /= 100; return; }
    if (!entry[0]) return;
    double v = parse(entry) / 100;
    /* 足し算・引き算のあとなら「前の数の何%」にする (100 + 10% = 110) */
    if (ntok && (op[ntok - 1] == u'+' || op[ntok - 1] == u'−')) v *= evaluate(num, op, ntok);
    format(v, entry, DIGITS + 3);
    c16 t[DIGITS + 3]; int k = 0; for (int i = 0; entry[i]; i++) if (entry[i] != u',') t[k++] = entry[i];
    t[k] = 0; scopy(entry, t, DIGITS + 3);
}

/* ---- ボタン ---- */
static const c16 *labels[5][4] = {
    {u"C", u"±", u"%", u"÷"},
    {u"7", u"8", u"9", u"×"},
    {u"4", u"5", u"6", u"−"},
    {u"1", u"2", u"3", u"+"},
    {u"0", u".", u"←", u"="},
};
static int pressed = -1, pressTicks;
static void press(int r, int c) {
    const c16 *l = labels[r][c];
    pressed = r * 4 + c; pressTicks = 6;
    if (l[0] >= u'0' && l[0] <= u'9') digit(l[0]);
    else if (l[0] == u'.') digit(u'.');
    else if (l[0] == u'C') clear_all();
    else if (l[0] == u'±') negate();
    else if (l[0] == u'%') percent();
    else if (l[0] == u'←') back_space();
    else if (l[0] == u'=') equals();
    else oper(l[0]);
    invalidate();
}
static void find(c16 ch) {
    for (int r = 0; r < 5; r++) for (int c = 0; c < 4; c++) if (labels[r][c][0] == ch) { press(r, c); return; }
}

#define PAD 10
#define DISP 92
static int bw, bh;
static void layout(int w, int h) { bw = (w - PAD * 5) / 4; bh = (h - DISP - PAD * 7) / 5; if (bh > 64) bh = 64; }

/* 大きな数字 (起動画面と同じ太字) */
static int big_text_w(const c16 *s) { int w = 0, gw, adv, gh; for (; *s; s++) w += (title_bitmap(*s, &gw, &adv, &gh) ? adv : 18) + 1; return w; }
static void big_text(int x, int y, const c16 *s, u32 color) {
    u32 *scr = screen();
    for (; *s; s++) {
        int gw, adv, gh; const u8 *b = title_bitmap(*s, &gw, &adv, &gh);
        if (!b) { x += 18; continue; }
        for (int j = 0; j < gh; j++) for (int i = 0; i < gw; i++) {
            u32 a = b[j * gw + i]; int px = x + i - 1, py = y + j;
            if (!a || px < 0 || py < 0 || px >= SW || py >= SH) continue;
            u32 *p = &scr[py * SW + px], d = *p;
            u32 r = (((color >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
            u32 g = (((color >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
            u32 bl = ((color & 255) * a + (d & 255) * (255 - a)) / 255;
            *p = r << 16 | g << 8 | bl;
        }
        x += adv + 1;
    }
}

void calc_open(void) { if (!ntok && !entry[0] && !shown) clear_all(); }
void calc_draw(int x, int y, int w, int h) {
    layout(w, h);
    fill(x, y, w, h, 0x1C1C2A);
    /* 表示部: 上に式、下に大きく答え */
    fill(x + PAD, y + PAD, w - PAD * 2, DISP - PAD, 0x101018);
    c16 line[160] = u"", v[40];
    for (int i = 0; i < ntok; i++) {
        format(num[i], v, 40); scat(line, v, 160);
        c16 o[4] = {u' ', op[i], u' ', 0}; scat(line, o, 160);
    }
    if (shown) { /* = のあとは式全体を出す */
        if (entry[0]) { format(parse(entry), v, 40); scat(line, v, 160); }
        scat(line, u" =", 160);
    }
    int lw = 0; for (int i = 0; line[i]; i++) lw += charw(line[i]);
    clip(x + PAD + 4, y + PAD, w - PAD * 2 - 8, 24);
    text(x + w - PAD - 8 - lw, y + PAD + 6, line, 0x9C9CC0);
    noclip();
    c16 big[48];
    if (error) scopy(big, u"エラー", 48);
    else if (shown) {
        /* 大きな文字に入りきるまで桁を減らす */
        for (sig = 12; sig >= 6; sig -= 2) { format(result, big, 48); if (big_text_w(big) <= w - PAD * 2 - 16) break; }
        sig = 12;
    }
    else if (entry[0]) {
        if (entry[0] == u'-' && !entry[1]) scopy(big, u"-", 48);
        else {
            /* 入力中の数もカンマ区切りで (小数点以下はそのまま) */
            int n = slen(entry), dot = n; for (int i = 0; i < n; i++) if (entry[i] == u'.') { dot = i; break; }
            int neg = entry[0] == u'-', o = 0;
            if (neg) big[o++] = u'-';
            for (int i = neg; i < dot; i++) { big[o++] = entry[i]; if ((dot - 1 - i) % 3 == 0 && i != dot - 1) big[o++] = u','; }
            for (int i = dot; i < n; i++) big[o++] = entry[i];
            big[o] = 0;
        }
    } else format(preview(), big, 48);
    u32 col = error ? 0xFF6B81 : 0xFFFFFF;
    int bwid = big_text_w(big);
    if (error || bwid > w - PAD * 2 - 16) {
        int tw = 0; for (int i = 0; big[i]; i++) tw += charw(big[i]);
        text(x + w - PAD - 8 - tw, y + PAD + 46, big, col);
    } else big_text(x + w - PAD - 8 - bwid, y + PAD + 32, big, col);

    for (int r = 0; r < 5; r++) for (int c = 0; c < 4; c++) {
        int bx = x + PAD + c * (bw + PAD), by = y + DISP + PAD + r * (bh + PAD);
        const c16 *l = labels[r][c];
        int isOp = c == 3, isFn = r == 0 && c < 3, isEq = l[0] == u'=';
        u32 bg = isEq ? 0x6D5DFC : isOp ? 0x3A3270 : isFn ? 0x3A3A52 : 0x2A2A3C;
        if (pressed == r * 4 + c && pressTicks) bg = isEq ? 0x9585FF : 0x55557A;
        fill(bx, by, bw, bh, bg);
        fill(bx, by + bh - 2, bw, 2, 0x15151F); /* 下に少し影 */
        int tw = 0; for (int i = 0; l[i]; i++) tw += charw(l[i]);
        int gw, adv, gh;
        if (l[1] == 0 && title_bitmap(l[0], &gw, &adv, &gh)) big_text(bx + (bw - adv) / 2, by + (bh - gh) / 2 - 1, l, 0xFFFFFF);
        else text(bx + (bw - tw) / 2, by + (bh - 16) / 2, l, 0xFFFFFF);
    }
}
void calc_key(Key k) {
    c16 ch = k.ch;
    if (ch >= u'0' && ch <= u'9') find(ch);
    else if (ch == u'.' || ch == u',') find(u'.');
    else if (ch == u'+') find(u'+');
    else if (ch == u'-') find(u'−');
    else if (ch == u'*' || ch == u'x' || ch == u'X') find(u'×');
    else if (ch == u'/') find(u'÷');
    else if (ch == u'%') find(u'%');
    else if (ch == u'=' || ch == 13) find(u'=');
    else if (ch == 8) find(u'←');
    else if (ch == u'c' || ch == u'C' || k.scan == 8 /* Delete */) find(u'C');
    else if (ch == u'n' || ch == u'N') find(u'±');
}
void calc_click(int x, int y) {
    for (int r = 0; r < 5; r++) for (int c = 0; c < 4; c++) {
        int bx = PAD + c * (bw + PAD), by = DISP + PAD + r * (bh + PAD);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) { press(r, c); return; }
    }
}
void calc_tick(void) { if (pressTicks && !--pressTicks) invalidate(); }
