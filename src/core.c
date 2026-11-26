/* FreeOS Any 中核: 起動、画面、入力、ファイル、ウィンドウ管理 */
#include "os.h"
#include "net.h"
#include "font.h"
#include "fontaa.h"
#include "icons.h"
#include "vkbd.h"

SystemTable *ST;
BootServices *BS;
int _fltused;

/* コンパイラが暗黙に呼ぶ関数 */
void *memmove(void *d, const void *s, u64 n) { mcopy(d, s, n); return d; }
int memcmp(const void *a, const void *b, u64 n) { const u8 *x = a, *y = b; for (; n; n--, x++, y++) if (*x != *y) return *x - *y; return 0; }
u64 strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
void *memset(void *d, int c, u64 n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, u64 n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
void mcopy(void *d, const void *s, u64 n) {
    u8 *p = d; const u8 *q = s;
    if (p <= q) while (n--) *p++ = *q++;
    else while (n) { n--; p[n] = q[n]; }
}

/* ---- 文字列 ---- */
int slen(const c16 *s) { int n = 0; while (s[n]) n++; return n; }
int seq(const c16 *a, const c16 *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
void scopy(c16 *d, const c16 *s, int cap) { int i = 0; for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }
void scat(c16 *d, const c16 *s, int cap) { int n = slen(d); scopy(d + n, s, cap - n); }
void snum(c16 *d, i64 v, int width, int cap) {
    c16 t[32]; int i = 31, neg = v < 0; u64 u = neg ? -(u64)v : (u64)v;
    t[i] = 0;
    do { t[--i] = u'0' + u % 10; u /= 10; width--; } while (u || width > 0);
    if (neg) t[--i] = u'-';
    scat(d, t + i, cap);
}

/* ---- 描画 ---- */
static Gop *gop;
static u32 *back, *scaled;
static int scale = 1, cx0, cy0, cx1, cy1;
int SW, SH;

void noclip(void) { cx0 = cy0 = 0; cx1 = SW; cy1 = SH; }
void clip(int x, int y, int w, int h) {
    cx0 = x < 0 ? 0 : x; cy0 = y < 0 ? 0 : y;
    cx1 = x + w > SW ? SW : x + w; cy1 = y + h > SH ? SH : y + h;
}
void fill(int x, int y, int w, int h, u32 color) {
    int x1 = x + w > cx1 ? cx1 : x + w, y1 = y + h > cy1 ? cy1 : y + h;
    if (x < cx0) x = cx0;
    if (y < cy0) y = cy0;
    for (; y < y1; y++) for (int i = x; i < x1; i++) back[y * SW + i] = color;
}
static int find_glyph(c16 ch) {
    int lo = 0, hi = FONT_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_code[mid] == ch) return mid;
        if (font_code[mid] < ch) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
int glyph_bits(c16 ch, const u16 **bits) {
    int g = find_glyph(ch);
    if (g < 0) g = find_glyph(u'?');
    *bits = font_bits[g];
    return font_width[g];
}
int charw(c16 ch) { int g = find_glyph(ch); return g < 0 ? 8 : font_width[g]; }
/* 濃さ a (0〜15) で色を混ぜる */
static void shade(int x, int y, u32 c, u32 a) {
    if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1) return;
    u32 *p = &back[y * SW + x], d = *p;
    if (a >= 15) { *p = c; return; }
    u32 r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * (15 - a)) / 15;
    u32 g = (((c >> 8) & 255) * a + ((d >> 8) & 255) * (15 - a)) / 15;
    u32 b = ((c & 255) * a + (d & 255) * (15 - a)) / 15;
    *p = r << 16 | g << 8 | b;
}
int glyph(int x, int y, c16 ch, u32 color) {
    int g = find_glyph(ch);
    if (g < 0) g = find_glyph(u'?');
    int w = font_width[g];
    u32 off = fontaa_off[g];
    if (off != 0xFFFFFFFF) { /* BIZ UDゴシック (なめらか) */
        const u8 *d = fontaa_data + off;
        for (int i = 0; i < 16 * w; i++) {
            u32 a = i & 1 ? d[i >> 1] & 15 : d[i >> 1] >> 4;
            if (a) shade(x + i % w, y + i / w, color, a);
        }
        return w;
    }
    for (int row = 0; row < 16; row++) /* Unifont */
        for (int col = 0; col < w; col++)
            if (font_bits[g][row] & (0x8000 >> col)) fill(x + col, y + row, 1, 1, color);
    return w;
}
/* 起動画面の名前用の大きな文字 (濃さ 0〜255、高さ TITLE_H) */
const u8 *title_bitmap(c16 ch, int *w, int *adv, int *h) {
    *h = TITLE_H;
    for (int i = 0; i < TITLE_N; i++)
        if (title_glyph[i].code == ch) { *w = title_glyph[i].w; *adv = title_glyph[i].adv; return title_data + title_glyph[i].off; }
    *w = *adv = 0;
    return 0;
}
int text(int x, int y, const c16 *s, u32 color) {
    while (*s) x += glyph(x, y, *s++, color);
    return x;
}
static void present(void) {
    if (scale == 1) { gop->Blt(gop, back, 2, 0, 0, 0, 0, SW, SH, 0); return; }
    int w = SW * scale;
    for (int y = 0; y < SH * scale; y++)
        for (int x = 0; x < w; x++) scaled[y * w + x] = back[(y / scale) * SW + x / scale];
    gop->Blt(gop, scaled, 2, 0, 0, 0, 0, w, SH * scale, 0);
}

/* ---- ファイル ---- */
static File *root;
static Guid gFileInfo = {0x09576e92, 0x6d3f, 0x11d2, {0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};

int fs_ready(void) { return root != 0; }
static void full(const c16 *path, c16 *out) { scopy(out, u"\\HOME", PATHLEN + 8); scat(out, path, PATHLEN + 8); }
static File *fs_open(const c16 *path, u64 mode, u64 attr) {
    c16 p[PATHLEN + 8]; File *f = 0;
    if (!root) return 0;
    full(path, p);
    return root->Open(root, &f, p, mode, attr) ? 0 : f;
}
int fs_resolve(const c16 *cwd, const c16 *arg, c16 *out) {
    int n = 0;
    if (arg[0] != u'\\' && arg[0] != u'/') { scopy(out, cwd, PATHLEN); n = slen(out); } else out[0] = 0;
    while (*arg) {
        c16 part[64]; int k = 0;
        while (*arg == u'/' || *arg == u'\\') arg++;
        while (*arg && *arg != u'/' && *arg != u'\\') {
            c16 c = *arg++;
            if (c < 32 || c == u':' || c == u'*' || c == u'?' || c == u'"' || c == u'<' || c == u'>' || c == u'|' || k >= 63) return 0;
            part[k++] = c;
        }
        part[k] = 0;
        if (!k || seq(part, u".")) continue;
        if (seq(part, u"..")) { /* \HOME より上には出ない */
            while (n && out[--n] != u'\\') {}
            out[n] = 0;
            continue;
        }
        if (part[k - 1] == u'.' || part[k - 1] == u' ') return 0; /* FAT で別名扱いになる名前 */
        if (n + k + 2 > PATHLEN) return 0;
        out[n++] = u'\\';
        scopy(out + n, part, PATHLEN - n);
        n += k;
    }
    return 1;
}
int fs_stat(const c16 *path, Entry *e) {
    File *f = fs_open(path, FILE_READ, 0);
    if (!f) return 0;
    static FileInfo info; u64 size = sizeof info;
    Status s = f->GetInfo(f, &gFileInfo, &size, &info);
    f->Close(f);
    if (s) return 0;
    if (e) { scopy(e->name, info.name, 64); e->size = info.fileSize; e->dir = (info.attr & FILE_DIRECTORY) != 0; }
    return 1;
}
int fs_list(const c16 *path, Entry *out, int max) {
    File *d = fs_open(path, FILE_READ, 0);
    if (!d) return -1;
    static FileInfo info; int n = 0;
    while (n < max) {
        u64 size = sizeof info;
        if (d->Read(d, &size, &info) || !size) break;
        if (seq(info.name, u".") || seq(info.name, u"..") || slen(info.name) > 63) continue;
        Entry e = { .size = info.fileSize, .dir = (info.attr & FILE_DIRECTORY) != 0 };
        scopy(e.name, info.name, 64);
        int i = n++; /* フォルダを先に、名前順 */
        for (; i > 0; i--) {
            Entry *p = &out[i - 1]; int less = e.dir > p->dir;
            if (e.dir == p->dir) { int k = 0; while (e.name[k] && e.name[k] == p->name[k]) k++; less = e.name[k] < p->name[k]; }
            if (!less) break;
            out[i] = *p;
        }
        out[i] = e;
    }
    d->Close(d);
    return n;
}
i64 fs_read(const c16 *path, u8 *buf, u64 cap) {
    Entry e;
    if (!fs_stat(path, &e) || e.dir || e.size > cap) return -1;
    File *f = fs_open(path, FILE_READ, 0);
    if (!f) return -1;
    u64 n = cap;
    Status s = f->Read(f, &n, buf);
    f->Close(f);
    return s || n != e.size ? -1 : (i64)n;
}
int fs_delete(const c16 *path) {
    File *f = fs_open(path, FILE_READ | FILE_WRITE, 0);
    if (!f) return 0;
    return f->Delete(f) == 0; /* Delete はハンドルも閉じる。空でないフォルダは失敗 */
}
int fs_move(const c16 *from, const c16 *to) {
    if (!from[0] || !to[0] || fs_stat(to, 0)) return 0;
    File *f = fs_open(from, FILE_READ | FILE_WRITE, 0);
    if (!f) return 0;
    static FileInfo info; u64 size = sizeof info;
    Status s = f->GetInfo(f, &gFileInfo, &size, &info);
    if (!s) {
        c16 p[PATHLEN + 8]; full(to, p);
        if (slen(p) > 255) s = 1;
        else {
            scopy(info.name, p, 256);
            info.size = 80 + (slen(p) + 1) * 2;
            s = f->SetInfo(f, &gFileInfo, info.size, &info);
        }
    }
    f->Close(f);
    return s == 0;
}
static int write_new(const c16 *path, const u8 *buf, u64 len) {
    File *f = fs_open(path, FILE_READ | FILE_WRITE | FILE_CREATE, 0);
    if (!f) return 0;
    u64 n = len;
    Status s = len ? f->Write(f, &n, (void *)buf) : 0;
    if (!s) s = f->Flush(f);
    f->Close(f);
    return !s && n == len;
}
/* 途中で失敗しても元の内容が残るように、別名に書いてから入れ替える */
int fs_write(const c16 *path, const u8 *buf, u64 len) {
    c16 tmp[PATHLEN], old[PATHLEN]; Entry e;
    if (!path[0] || slen(path) > PATHLEN - 5) return 0;
    int exists = fs_stat(path, &e);
    if (exists && e.dir) return 0;
    scopy(tmp, path, PATHLEN); scat(tmp, u".new", PATHLEN);
    scopy(old, path, PATHLEN); scat(old, u".old", PATHLEN);
    if (fs_stat(tmp, 0) && !fs_delete(tmp)) return 0;
    if (!write_new(tmp, buf, len)) return 0;
    if (exists) {
        if (fs_stat(old, 0) && !fs_delete(old)) return 0;
        if (!fs_move(path, old)) return 0;
    }
    if (!fs_move(tmp, path)) { if (exists) fs_move(old, path); return 0; }
    if (exists) fs_delete(old);
    return 1;
}
int fs_mkdir(const c16 *path) {
    if (fs_stat(path, 0)) return 0;
    File *f = fs_open(path, FILE_READ | FILE_WRITE | FILE_CREATE, FILE_DIRECTORY);
    if (!f) return 0;
    f->Close(f);
    return 1;
}
int fs_trash(const c16 *path) {
    if (!path[0] || seq(path, u"\\TRASH")) return 0;
    const c16 *name = path;
    for (const c16 *p = path; *p; p++) if (*p == u'\\') name = p + 1;
    for (int i = 1; i < 1000; i++) {
        c16 to[PATHLEN] = u"\\TRASH\\";
        if (i > 1) { snum(to, i, 0, PATHLEN); scat(to, u"_", PATHLEN); }
        scat(to, name, PATHLEN);
        if (!fs_stat(to, 0)) return fs_move(path, to);
    }
    return 0;
}
int utf8_decode(const u8 *in, u64 n, c16 *out, int cap) {
    int m = 0;
    for (u64 i = 0; i < n;) {
        u32 c = in[i++]; int more = 0;
        if (c >= 0xF0) return -1; /* UCS-2 に入らない文字 */
        if (c >= 0xE0) { c &= 15; more = 2; }
        else if (c >= 0xC0) { c &= 31; more = 1; }
        else if (c >= 0x80) return -1;
        while (more--) { if (i >= n || (in[i] & 0xC0) != 0x80) return -1; c = c << 6 | (in[i++] & 63); }
        if (!c || m >= cap) return -1;
        out[m++] = (c16)c;
    }
    return m;
}
int utf8_encode(const c16 *in, int n, u8 *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        c16 c = in[i];
        if (c < 0x80) out[m++] = c;
        else if (c < 0x800) { out[m++] = 0xC0 | c >> 6; out[m++] = 0x80 | (c & 63); }
        else { out[m++] = 0xE0 | c >> 12; out[m++] = 0x80 | ((c >> 6) & 63); out[m++] = 0x80 | (c & 63); }
    }
    return m;
}
void fs_init(Handle image) {
    Guid gLoaded = {0x5B1B31A1, 0x9562, 0x11d2, {0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
    Guid gFs = {0x964e5b22, 0x6459, 0x11d2, {0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
    LoadedImage *li; FileSystem *sfs; File *vol, *home;
    if (BS->HandleProtocol(image, &gLoaded, (void **)&li)) return;
    if (BS->HandleProtocol(li->device, &gFs, (void **)&sfs)) return;
    if (sfs->OpenVolume(sfs, &vol)) return;
    if (vol->Open(vol, &home, u"\\HOME", FILE_READ | FILE_WRITE | FILE_CREATE, FILE_DIRECTORY)) return;
    home->Close(home);
    root = vol;
    fs_mkdir(u"\\TRASH");
    if (!fs_stat(u"\\README.TXT", 0)) {
        static u8 buf[2048];
        const c16 *s =
            u"FreeOS Anyへようこそ。\n\n"
            u"Esc スタートメニュー  F1 ウィンドウ切替  F4 閉じる\n"
            u"F5 ターミナル  F6 ファイル  F7 エディター\n"
            u"F8 ブラウザ    F9 ファイル転送  F10 最大化\n\n"
            u"電源を切るには、スタートメニューでシャットダウンを選びます。\n"
            u"ファイルは起動したUSBメモリの HOME フォルダに保存されます。\n";
        fs_write(u"\\README.TXT", buf, utf8_encode(s, slen(s), buf));
    }
}

/* ---- ウィンドウ管理 ---- */
#define TITLE 24
#define DOCK 48
typedef struct { int open, max, min, x, y, w, h; } Win; /* min: 最小化 (タスクバーのアイコンだけ) */
static Win wins[APP_COUNT];
static int order[APP_COUNT], nwin; /* 後ろほど手前 */
static int dirty = 1, mx, my, mdown, drag = -1, dragx, dragy, hasPointer;
static int busyApp = -1, cancelReq; /* 通信中のアプリ: そのアプリへの操作は Esc (中止) だけ。ほかのアプリは動かし続ける */
/* サイズ変更中の窓と、つかんだ縁 (1 左 / 2 右 / 4 下) */
static int resizing = -1, redges, rsx, rsy, rx0, ry0, rw0, rh0;
static u64 lastClick; static int lastClickApp = -1;
#define MINW 200
#define MINH 120
static c16 note[160];
static int noteTicks;

void invalidate(void) { dirty = 1; }
void notify(const c16 *msg) { scopy(note, msg, 160); noteTicks = 150; dirty = 1; }
/* 一番手前に見えているアプリ (なければ -1) */
static int top_app(void) { return nwin && !wins[order[nwin - 1]].min ? order[nwin - 1] : -1; }
static void raise(int app) {
    wins[app].min = 0;
    int i = 0;
    while (i < nwin && order[i] != app) i++;
    if (i == nwin) return;
    for (; i < nwin - 1; i++) order[i] = order[i + 1];
    order[nwin - 1] = app;
    dirty = 1;
}
void wm_open(int a) {
    if (!wins[a].open) {
        Win *w = &wins[a];
        w->open = 1; w->max = 0;
        w->w = apps[a].w; w->h = apps[a].h;
        w->x = 40 + (a % 5) * 56; w->y = 24 + (a % 5) * 34;
        if (w->x + apps[a].w > SW) w->x = SW - apps[a].w > 0 ? SW - apps[a].w : 0;
        if (w->y + apps[a].h > SH - DOCK) w->y = 0;
        order[nwin++] = a;
        if (apps[a].open) apps[a].open();
    }
    raise(a);
}
/* 最小化: 一番奥へ回して隠す */
static void wm_minimize(int a) {
    if (!wins[a].open) return;
    int i = 0;
    while (i < nwin && order[i] != a) i++;
    for (; i > 0; i--) order[i] = order[i - 1];
    order[0] = a;
    wins[a].min = 1;
    dirty = 1;
}
void wm_close(int a) {
    if (!wins[a].open) return;
    if (apps[a].flush) apps[a].flush();
    raise(a);
    nwin--;
    wins[a].open = wins[a].min = 0;
    dirty = 1;
}
int wm_active(int a) { return top_app() == a; }
void power(int reboot) {
    for (int a = 0; a < APP_COUNT; a++) if (wins[a].open && apps[a].flush) apps[a].flush();
    ST->rt->ResetSystem(reboot ? 0 : 2, 0, 0, 0);
}
static void geometry(int a, int *x, int *y, int *w, int *h) {
    if (wins[a].max) { *x = 0; *y = 0; *w = SW; *h = SH - DOCK; return; }
    *x = wins[a].x; *y = wins[a].y;
    *w = wins[a].w > SW ? SW : wins[a].w;
    *h = wins[a].h > SH - DOCK ? SH - DOCK : wins[a].h;
}
/* アイコン (ARGB) を描く。size は 32 / 20 / 16 */
#define ICON_LOGO (ICON_N - 1)
static void icon_draw(int id, int size, int x0, int y0) {
    const u32 *px = size == 32 ? icon32[id] : size == 20 ? icon20[id] : icon16[id];
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
        u32 c = px[y * size + x], a = c >> 24;
        int X = x0 + x, Y = y0 + y;
        if (!a || X < cx0 || X >= cx1 || Y < cy0 || Y >= cy1) continue;
        u32 *p = &back[Y * SW + X], d = *p;
        if (a == 255) { *p = c & 0xFFFFFF; continue; }
        u32 r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
        u32 g = (((c >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
        u32 b = ((c & 255) * a + (d & 255) * (255 - a)) / 255;
        *p = r << 16 | g << 8 | b;
    }
}
/* 角を丸めた四角 (半径 r) */
static void fill_round(int x, int y, int w, int h, int r, u32 color) {
    for (int j = 0; j < h; j++) {
        int inset = 0, dy = j < r ? r - j : j >= h - r ? j - (h - r - 1) : 0;
        if (dy) { while (inset < r && (r - inset) * (r - inset) + dy * dy > r * r + r) inset++; }
        fill(x + inset, y + j, w - inset * 2, 1, color);
    }
}

/* ---- スタートメニュー ---- */
#define MENU_N (APP_COUNT + 2)
#define MENU_W 230
#define MENU_ROW 30
#define START_W 112
static int menu, msel;
static const c16 *menuKey[APP_COUNT] = {u"F5", u"F6", u"F7", u"F8", u"F9"};
static int menu_y(void) { return SH - DOCK - MENU_N * MENU_ROW - 10; }
static void menu_run(int i) {
    menu = 0; dirty = 1;
    if (i < APP_COUNT) wm_open(i);
    else power(i == APP_COUNT);
}
static void menu_draw(void) {
    int y = menu_y();
    fill(4, y, MENU_W, MENU_N * MENU_ROW + 8, 0x7C6CFF);
    fill(5, y + 1, MENU_W - 2, MENU_N * MENU_ROW + 6, 0x1E1E2A);
    for (int i = 0; i < MENU_N; i++) {
        int py = y + 4 + i * MENU_ROW;
        if (i == msel) fill(7, py, MENU_W - 6, MENU_ROW, 0x6D5DFC);
        if (i == APP_COUNT) fill(12, py, MENU_W - 16, 1, 0x55556A);
        if (i < APP_COUNT) icon_draw(i, 20, 14, py + 5);
        text(i < APP_COUNT ? 44 : 20, py + 7, i < APP_COUNT ? apps[i].title : i == APP_COUNT ? u"再起動" : u"シャットダウン", 0xFFFFFF);
        if (i < APP_COUNT && menuKey[i]) text(MENU_W - 28, py + 7, menuKey[i], 0xB0B0D0);
    }
}
/* タスクバー: すべてのアプリのアイコンを並べる */
#define SLOT 48
static int slot_x(int a) { return START_W + 8 + a * SLOT; }
static int slot_at(int x, int y) {
    if (y < SH - DOCK) return -1;
    for (int a = 0; a < APP_COUNT; a++) if (x >= slot_x(a) && x < slot_x(a) + SLOT) return a;
    return -1;
}
/* アイコンを押したとき: 起動していなければ起動、裏なら手前へ、手前なら最小化 */
static void dock_press(int a) {
    if (!wins[a].open) wm_open(a);
    else if (top_app() == a) wm_minimize(a);
    else raise(a);
    dirty = 1;
}

/* マウスカーソル: 白地に黒い縁の矢印と、うすい影 */
static const char *const arrow[] = {
    "B...........", "BB..........", "BWB.........", "BWWB........", "BWWWB.......",
    "BWWWWB......", "BWWWWWB.....", "BWWWWWWB....", "BWWWWWWWB...", "BWWWWWWWWB..",
    "BWWWWWBBBBB.", "BWWBWWB.....", "BWB.BWWB....", "BB..BWWB....", "B....BWWB...",
    ".....BWWB...", "......BWWB..", "......BWWB..", ".......BB...",
};
static void cursor_draw(int x0, int y0) {
    for (int y = 0; y < 19; y++) for (int x = 0; x < 12; x++) { /* 影 */
        int px = x0 + x + 1, py = y0 + y + 2;
        if (arrow[y][x] == '.' || px < 0 || py < 0 || px >= SW || py >= SH) continue;
        u32 *p = &back[py * SW + px];
        *p = ((*p >> 2) & 0x3F3F3F) * 3; /* 25% 暗く */
    }
    for (int y = 0; y < 19; y++) for (int x = 0; x < 12; x++) {
        char c = arrow[y][x];
        if (c != '.') fill(x0 + x, y0 + y, 1, 1, c == 'B' ? 0x000000 : 0xFFFFFF);
    }
}

static int edge_at(int px, int py, int *app);
/* サイズ変更のカーソル: 白地に黒い縁の両矢印 (なめらかな縁と影)。向きは (ux, uy) */
static float csqrt(float x) {
    if (x <= 0) return 0;
#if defined(__aarch64__)
    float r; __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x)); return r;
#else
    float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r;
#endif
}
static float tri_d(float px, float py, const float *t) { /* 三角形までの距離 (内側は負、おおよそ) */
    float d = -1e9f;
    for (int i = 0; i < 3; i++) {
        float ax = t[i * 2], ay = t[i * 2 + 1], bx = t[(i + 1) % 3 * 2], by = t[(i + 1) % 3 * 2 + 1];
        float ex = bx - ax, ey = by - ay, l = csqrt(ex * ex + ey * ey);
        float s = -((px - ax) * ey - (py - ay) * ex) / l; /* 左側が外 */
        if (s > d) d = s;
    }
    return d;
}
static float arrow_d(float u, float v) {
    const float L = 10, H = 7, W = 6.5f;
    float au = u < 0 ? -u : u, av = v < 0 ? -v : v;
    float box = au - (L - H) > av - 2.6f ? au - (L - H) : av - 2.6f;
    float t[6] = {L, 0, L - H, -W, L - H, W};
    float tri = tri_d(au, v, t);
    return box < tri ? box : tri;
}
static void cblend(int x, int y, u32 c, float a) {
    if (a <= 0 || x < 0 || y < 0 || x >= SW || y >= SH) return;
    if (a > 1) a = 1;
    u32 *p = &back[y * SW + x], d = *p;
    u32 r = (u32)(((c >> 16) & 255) * a + ((d >> 16) & 255) * (1 - a));
    u32 g = (u32)(((c >> 8) & 255) * a + ((d >> 8) & 255) * (1 - a));
    u32 b = (u32)((c & 255) * a + (d & 255) * (1 - a));
    *p = r << 16 | g << 8 | b;
}
static void resize_cursor_draw(int cx, int cy, int e) {
    float ux = 1, uy = 0;
    if ((e & 3) && (e & 4)) { ux = 0.7071f; uy = e & 2 ? 0.7071f : -0.7071f; }
    else if (e & 4) { ux = 0; uy = 1; }
    for (int y = -13; y <= 13; y++) for (int x = -13; x <= 13; x++) {
        float px = x + 0.5f, py = y + 0.5f;
        float sx = px - 1, sy = py - 1.5f; /* 影 */
        float ds = arrow_d(sx * ux + sy * uy, -sx * uy + sy * ux);
        float sh = 1.6f - ds; sh = sh < 0 ? 0 : sh > 1.6f ? 1.6f : sh;
        cblend(cx + x, cy + y, 0x000000, sh * 0.18f);
    }
    for (int y = -13; y <= 13; y++) for (int x = -13; x <= 13; x++) {
        float px = x + 0.5f, py = y + 0.5f;
        float d = arrow_d(px * ux + py * uy, -px * uy + py * ux);
        float outer = 0.5f - d, inner = 0.5f - (d + 1.3f);
        cblend(cx + x, cy + y, 0x000000, outer);
        cblend(cx + x, cy + y, 0xFFFFFF, inner);
    }
}

u32 *screen(void) { return back; }
void screen_show(void) { present(); }
static void compose(void);
void desktop_compose(void) { compose(); }
static void draw(void) { compose(); present(); }
static void compose(void) {
    noclip();
    for (int y = 0; y < SH; y++) { /* 壁紙 */
        u32 r = 24 + 90 * y / SH, g = 26 + 40 * y / SH, b = 62 + 100 * y / SH;
        fill(0, y, SW, 1, r << 16 | g << 8 | b);
    }
    text(SW - 120, 16, u"FreeOS Any " FREEOS_VER, 0xB8B8E0);
    if (!nwin) text(24, 24, u"Esc かスタートボタンでメニューを開きます", 0xFFFFFF);
    for (int i = 0; i < nwin; i++) {
        int a = order[i], x, y, w, h, top = a == top_app();
        if (wins[a].min) continue;
        geometry(a, &x, &y, &w, &h);
        noclip();
        fill(x + 5, y + 5, w, h, 0x15152A);
        fill(x, y, w, h, top ? 0x7C6CFF : 0x55556A);
        fill(x + 1, y + TITLE, w - 2, h - TITLE - 1, 0xF4F4FA);
        icon_draw(a, 16, x + 6, y + 4);
        text(x + 28, y + 4, apps[a].title, 0xFFFFFF);
        text(x + w - 18, y + 4, u"×", 0xFFFFFF);
        fill(x + w - 45, y + 12, 10, 2, 0xFFFFFF); /* 最小化 */
        if (wins[a].max) { /* 元に戻す: 重なった四角 */
            fill(x + w - 66, y + 7, 8, 1, 0xFFFFFF); fill(x + w - 59, y + 7, 1, 8, 0xFFFFFF);
            fill(x + w - 69, y + 9, 9, 1, 0xFFFFFF); fill(x + w - 69, y + 17, 9, 1, 0xFFFFFF);
            fill(x + w - 69, y + 9, 1, 9, 0xFFFFFF); fill(x + w - 61, y + 9, 1, 9, 0xFFFFFF);
        } else { /* 最大化: 四角 */
            fill(x + w - 70, y + 7, 11, 1, 0xFFFFFF); fill(x + w - 70, y + 8, 11, 1, 0xFFFFFF);
            fill(x + w - 70, y + 17, 11, 1, 0xFFFFFF);
            fill(x + w - 70, y + 7, 1, 11, 0xFFFFFF); fill(x + w - 60, y + 7, 1, 11, 0xFFFFFF);
        }
        clip(x + 1, y + TITLE, w - 2, h - TITLE - 1);
        apps[a].draw(x + 1, y + TITLE, w - 2, h - TITLE - 1);
    }
    noclip();
    fill(0, SH - DOCK, SW, DOCK, 0x16161F);
    fill(0, SH - DOCK, SW, 1, 0x2A2A3A);
    fill_round(6, SH - DOCK + 7, START_W - 12, DOCK - 14, 8, menu ? 0x9585FF : 0x6D5DFC);
    icon_draw(ICON_LOGO, 20, 14, SH - DOCK + 14);
    text(40, SH - DOCK + 16, u"スタート", 0xFFFFFF);
    int hover = hasPointer && !menu ? slot_at(mx, my) : -1;
    for (int a = 0; a < APP_COUNT; a++) {
        int x = slot_x(a), y = SH - DOCK;
        if (a == top_app()) fill_round(x + 3, y + 3, SLOT - 6, DOCK - 6, 8, 0x3A3270);
        else if (a == hover) fill_round(x + 3, y + 3, SLOT - 6, DOCK - 6, 8, 0x2A2A3C);
        icon_draw(a, 32, x + (SLOT - 32) / 2, y + 6);
        if (wins[a].open) { /* 起動中の印 */
            int w = a == top_app() ? 16 : 6;
            fill_round(x + (SLOT - w) / 2, y + DOCK - 5, w, 3, 1, a == top_app() ? 0xFFFFFF : 0x9C9CC0);
        }
    }
    if (hover >= 0) { /* アプリ名の札 */
        int tw = 0; for (const c16 *p = apps[hover].title; *p; p++) tw += charw(*p);
        int bx = slot_x(hover) + SLOT / 2 - tw / 2 - 10; if (bx < 4) bx = 4;
        fill_round(bx, SH - DOCK - 34, tw + 20, 28, 6, 0x0E0E16);
        text(bx + 10, SH - DOCK - 28, apps[hover].title, 0xFFFFFF);
    }
    Time t;
    if (!ST->rt->GetTime(&t, 0)) {
        c16 s[24] = u"";
        snum(s, t.month, 0, 24); scat(s, u"/", 24); snum(s, t.day, 0, 24); scat(s, u" ", 24);
        snum(s, t.hour, 2, 24); scat(s, u":", 24); snum(s, t.minute, 2, 24);
        text(SW - 100, SH - DOCK + 16, s, 0xFFFFFF);
    }
    if (noteTicks) {
        int w = 24; for (int i = 0; note[i]; i++) w += charw(note[i]);
        fill(SW - w - 12, SH - DOCK - 40, w, 28, 0x101018);
        text(SW - w, SH - DOCK - 34, note, 0xFFFFFF);
    }
    if (menu) menu_draw();
    if (hasPointer) {
        int ea, e = resizing >= 0 ? redges : drag < 0 && !menu ? edge_at(mx, my, &ea) : 0;
        if (e) resize_cursor_draw(mx, my, e); else cursor_draw(mx, my);
    }
}

void redraw(void) { draw(); dirty = 1; }
/* マウスの下の窓の縁 (1 左 / 2 右 / 4 下)。a に窓を返す */
static int edge_at(int px, int py, int *app) {
    for (int i = nwin - 1; i >= 0; i--) {
        int a = order[i], x, y, w, h;
        if (wins[a].min) continue;
        geometry(a, &x, &y, &w, &h);
        if (px < x - 4 || py < y || px >= x + w + 4 || py >= y + h + 4) continue;
        if (wins[a].max) return 0;
        int e = 0;
        if (py >= y + TITLE) { if (px < x + 4) e |= 1; if (px >= x + w - 4) e |= 2; }
        if (py >= y + h - 4) e |= 4;
        if ((e & 4) && px < x + 12) e |= 1;          /* 下の角は少し広く */
        if ((e & 4) && px >= x + w - 12) e |= 2;
        if (e) *app = a;
        return e;
    }
    return 0;
}
static void toggle_max(int a) { wins[a].max ^= 1; dirty = 1; }
static void press(void) {
    if (menu) {
        int i = (my - menu_y() - 4) / MENU_ROW;
        if (mx < MENU_W && my >= menu_y() + 4 && i < MENU_N) { menu_run(i); return; }
        menu = 0;
        if (my >= SH - DOCK && mx < START_W) return;
    }
    if (my >= SH - DOCK) {
        if (mx < START_W) { menu = 1; msel = 0; return; }
        int a = slot_at(mx, my);
        if (a >= 0) dock_press(a);
        return;
    }
    int ea, e = edge_at(mx, my, &ea);
    if (e) { /* 縁をつかんでサイズ変更 */
        raise(ea);
        resizing = ea; redges = e; rsx = mx; rsy = my;
        rx0 = wins[ea].x; ry0 = wins[ea].y; rw0 = wins[ea].w; rh0 = wins[ea].h;
        return;
    }
    for (int i = nwin - 1; i >= 0; i--) {
        int a = order[i], x, y, w, h;
        if (wins[a].min) continue;
        geometry(a, &x, &y, &w, &h);
        if (mx < x || my < y || mx >= x + w || my >= y + h) continue;
        raise(a);
        if (my < y + TITLE) {
            u64 now = net_ms();
            int dbl = lastClickApp == a && now - lastClick < 400;
            lastClick = now; lastClickApp = a;
            if (mx >= x + w - 26) { if (a != busyApp) wm_close(a); }   /* 通信中は閉じない */
            else if (mx >= x + w - 52) wm_minimize(a);
            else if (mx >= x + w - 78 || dbl) { toggle_max(a); lastClickApp = -1; }
            else if (!wins[a].max) { drag = a; dragx = mx - x; dragy = my - y; }
        } else if (apps[a].click && a != busyApp) apps[a].click(mx - x - 1, my - y - TITLE);
        return;
    }
}
static void pointer_update(int x, int y, int down) {
    x = x < 0 ? 0 : x >= SW ? SW - 1 : x;
    y = y < 0 ? 0 : y >= SH ? SH - 1 : y;
    if (x != mx || y != my || down != mdown) dirty = 1;
    mx = x; my = y;
    if (menu && mx < MENU_W && my >= menu_y() + 4 && my < menu_y() + 4 + MENU_N * MENU_ROW) msel = (my - menu_y() - 4) / MENU_ROW;
    int pressed = down && !mdown;
    mdown = down; /* 先に更新する (押した処理の中で読み込み中に OS が回っても、同じクリックを二度扱わない) */
    if (pressed) press();
    if (!down) drag = resizing = -1;
    if (resizing >= 0) {
        Win *w = &wins[resizing];
        int dx = mx - rsx, dy = my - rsy;
        if (redges & 2) w->w = rw0 + dx < MINW ? MINW : rw0 + dx > SW ? SW : rw0 + dx;
        if (redges & 1) { int nw = rw0 - dx < MINW ? MINW : rw0 - dx; w->x = rx0 + rw0 - nw; w->w = nw; }
        if (redges & 4) w->h = rh0 + dy < MINH ? MINH : ry0 + rh0 + dy > SH - DOCK ? SH - DOCK - ry0 : rh0 + dy;
        dirty = 1;
    }
    if (drag >= 0) {
        int nx = mx - dragx, ny = my - dragy;
        wins[drag].x = nx < 60 - apps[drag].w ? 60 - apps[drag].w : nx > SW - 60 ? SW - 60 : nx;
        wins[drag].y = ny < 0 ? 0 : ny > SH - DOCK - TITLE ? SH - DOCK - TITLE : ny;
    }
}
/* ホイール: マウスの下の窓へ */
void pointer_wheel(int dy) {
    if (!dy) return;
    for (int i = nwin - 1; i >= 0; i--) {
        int a = order[i], x, y, w, h;
        if (wins[a].min) continue;
        geometry(a, &x, &y, &w, &h);
        if (mx < x || my < y || mx >= x + w || my >= y + h) continue;
        if (a == busyApp) return;
        if (apps[a].wheel) apps[a].wheel(dy);
        else if (apps[a].key) for (int k = 0; k < 3 * (dy < 0 ? -dy : dy); k++) { Key kk = {(u16)(dy < 0 ? 1 : 2), 0}; apps[a].key(kk); }
        dirty = 1;
        return;
    }
}
void pointer_abs(int x, int y, int down) {
    hasPointer = 1;
    pointer_update((int)((i64)x * SW / 32768), (int)((i64)y * SH / 32768), down);
}

#define MAXPTR 8
static Pointer *rel[MAXPTR]; static AbsPointer *abso[MAXPTR];
static int nrel, nabs;
static void pointer_init(void) {
    Guid gRel = {0x31878c87, 0x0b75, 0x11d5, {0x9a,0x4f,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
    Guid gAbs = {0x8D59D32B, 0xC655, 0x4AE9, {0x9B,0x15,0xF2,0x59,0x04,0x99,0x2A,0x43}};
    u64 n; Handle *h;
    if (!BS->LocateHandleBuffer(2, &gRel, 0, &n, &h)) {
        for (u64 i = 0; i < n && nrel < MAXPTR; i++) if (!BS->HandleProtocol(h[i], &gRel, (void **)&rel[nrel])) { rel[nrel]->Reset(rel[nrel], 0); nrel++; }
        BS->FreePool(h);
    }
    if (!BS->LocateHandleBuffer(2, &gAbs, 0, &n, &h)) {
        for (u64 i = 0; i < n && nabs < MAXPTR; i++) if (!BS->HandleProtocol(h[i], &gAbs, (void **)&abso[nabs])) nabs++;
        BS->FreePool(h);
    }
}
static void pointer_poll(void) {
    for (int i = 0; i < nrel; i++) {
        PointerState s;
        if (rel[i]->GetState(rel[i], &s)) continue;
        i64 rx = rel[i]->Mode->resx ? rel[i]->Mode->resx : 1, ry = rel[i]->Mode->resy ? rel[i]->Mode->resy : 1;
        int dx = (int)(s.x * 4 / rx), dy = (int)(s.y * 4 / ry);
        if (!dx && s.x) dx = s.x > 0 ? 1 : -1;
        if (!dy && s.y) dy = s.y > 0 ? 1 : -1;
        hasPointer = 1;
        if (s.z) pointer_wheel(s.z > 0 ? -1 : 1);
        pointer_update(mx + dx, my + dy, s.left);
    }
    for (int i = 0; i < nabs; i++) {
        AbsState s; AbsMode *m = abso[i]->Mode;
        if (m->maxx <= m->minx || m->maxy <= m->miny || abso[i]->GetState(abso[i], &s)) continue;
        hasPointer = 1;
        pointer_update((int)((s.x - m->minx) * SW / (m->maxx - m->minx)), (int)((s.y - m->miny) * SH / (m->maxy - m->miny)), s.buttons & 1);
    }
}

void ui_busy(int app) { busyApp = app; cancelReq = 0; dirty = 1; }
int ui_is_busy(int app) { return busyApp >= 0 && busyApp == app; }
static void key(Key k) {
    int top = top_app();
    dirty = 1;
    if (busyApp >= 0 && top == busyApp) {                                 /* 通信中のアプリが手前 */
        if (k.scan == 0x17) { cancelReq = 1; return; }                    /* Esc で中止 */
        if (k.scan == 0x0B || (k.scan >= 0x0F && k.scan <= 0x13)) {}      /* 窓の切り替え・ほかのアプリは開ける */
        else return;
    }
    if (k.scan == 0x17) { menu ^= 1; msel = 0; return; }                 /* Esc */
    if (menu) {
        if (k.scan == 1) msel = (msel + MENU_N - 1) % MENU_N;
        else if (k.scan == 2) msel = (msel + 1) % MENU_N;
        else if (k.ch == 13) menu_run(msel);
        else if (k.scan) menu = 0;
        if (menu || !k.scan) return;
    }
    if (k.scan == 0x0B) { if (nwin > 1) raise(order[0]); }               /* F1 */
    else if (k.scan == 0x0E) { if (top >= 0 && top != busyApp) wm_close(top); } /* F4 (通信中は閉じない) */
    else if (k.scan >= 0x0F && k.scan <= 0x13) wm_open(k.scan - 0x0F);   /* F5-F9 */
    else if (k.scan == 0x14) { if (top >= 0) toggle_max(top); }          /* F10 */
    else if (top >= 0 && apps[top].key) apps[top].key(k);
}

static Status die(const c16 *msg) {
    struct { void *reset; Status (*out)(void *, const c16 *); } *con = ST->conOut;
    con->out(con, msg);
    BS->Stall(10000000);
    return 1;
}

/* 通信の待ち時間に呼ばれる: 入力・ほかのアプリ・描き直しを 1 回分進める。中止なら 0 */
int ui_pump(void) {
    static u64 last; static int inPump;
    if (inPump || !back) return 1;
    inPump = 1;
    Key k; int special;
    for (int i = 0; i < 16 && vkbd_read(&k, &special); i++) { if (special) { k.scan = special == 1 ? K_KANA : K_EISU; k.ch = 0; } key(k); }
    for (int i = 0; i < 16 && !ST->conIn->ReadKeyStroke(ST->conIn, &k); i++) key(k);
    pointer_poll(); tablet_poll(); sound_poll();
    u64 now = net_ms();
    if (now - last >= 40) { /* 25 回/秒まで */
        last = now;
        for (int i = 0; i < nwin; i++) if (order[i] != busyApp && apps[order[i]].tick) apps[order[i]].tick();
        if (noteTicks && !--noteTicks) dirty = 1;
        if (busyApp >= 0) dirty = 1; /* 読み込み中の印を動かす */
        if (dirty) { dirty = 0; draw(); }
    }
    inPump = 0;
    if (cancelReq) { cancelReq = 0; return 0; }
    return 1;
}

Status efi_main(Handle image, SystemTable *st) {
    ST = st; BS = st->bs;
    BS->SetWatchdogTimer(0, 0, 0, 0); /* 5分で勝手に再起動されるのを止める */
    Guid gGop = {0x9042a9de, 0x23dc, 0x4a38, {0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
    if (BS->LocateProtocol(&gGop, 0, (void **)&gop)) return die(u"Graphics output not found.\r\n");
    int pw = gop->Mode->info->width, ph = gop->Mode->info->height;
    scale = pw >= 2400 && ph >= 1400 ? 2 : 1;
    SW = pw / scale; SH = ph / scale;
    if (BS->AllocatePool(2, (u64)SW * SH * 4, (void **)&back)) return die(u"Out of memory.\r\n");
    if (scale > 1 && BS->AllocatePool(2, (u64)pw * ph * 4, (void **)&scaled)) return die(u"Out of memory.\r\n");
    mx = SW / 2; my = SH / 2;
    fs_init(image);
    pointer_init();
    tablet_init();
    vkbd_init();
    sound_init();
    net_init(image);
    wm_open(APP_TERM);
    if (fs_stat(u"\\README.TXT", 0)) edit_open(u"\\README.TXT");
    if (!fs_ready()) notify(u"保存先がありません。ファイル機能は使えません");
    boot_splash();

    Event timer;
    BS->CreateEvent(0x80000000, 0, 0, 0, &timer);
    BS->SetTimer(timer, 1, 200000); /* 20ms 周期 */
    int minute = -1, ticks = 0;
    for (;;) {
        u64 which;
        BS->WaitForEvent(1, &timer, &which);
        Key k; int special;
        for (int i = 0; i < 16 && vkbd_read(&k, &special); i++) { /* 仮想マシンのキーボード (「かな」「英数」も) */
            if (special) { k.scan = special == 1 ? K_KANA : K_EISU; k.ch = 0; }
            key(k);
        }
        for (int i = 0; i < 16 && !ST->conIn->ReadKeyStroke(ST->conIn, &k); i++) key(k);
        pointer_poll();
        tablet_poll();
        sound_poll();
        net_poll();
        for (int i = 0; i < nwin; i++) if (apps[order[i]].tick) apps[order[i]].tick();
        if (noteTicks && !--noteTicks) dirty = 1;
        if (++ticks % 50 == 0) { Time t; if (!ST->rt->GetTime(&t, 0) && t.minute != minute) { minute = t.minute; dirty = 1; } }
        if (dirty) { dirty = 0; draw(); }
    }
}
