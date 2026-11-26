/* FreeOS Any 中核: 起動、画面、入力、ファイル、ウィンドウ管理 */
#include "os.h"
#include "net.h"
#include "font.h"

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
int charw(c16 ch) { int g = find_glyph(ch); return g < 0 ? 8 : font_width[g]; }
int glyph(int x, int y, c16 ch, u32 color) {
    int g = find_glyph(ch);
    if (g < 0) g = find_glyph(u'?');
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < font_width[g]; col++)
            if (font_bits[g][row] & (0x8000 >> col)) fill(x + col, y + row, 1, 1, color);
    return font_width[g];
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
#define DOCK 36
typedef struct { int open, max, x, y; } Win;
static Win wins[APP_COUNT];
static int order[APP_COUNT], nwin; /* 後ろほど手前 */
static int dirty = 1, mx, my, mdown, drag = -1, dragx, dragy, hasPointer;
static c16 note[160];
static int noteTicks;

void invalidate(void) { dirty = 1; }
void notify(const c16 *msg) { scopy(note, msg, 160); noteTicks = 150; dirty = 1; }
static void raise(int app) {
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
        w->x = 40 + (a % 5) * 56; w->y = 24 + (a % 5) * 34;
        if (w->x + apps[a].w > SW) w->x = SW - apps[a].w > 0 ? SW - apps[a].w : 0;
        if (w->y + apps[a].h > SH - DOCK) w->y = 0;
        order[nwin++] = a;
        if (apps[a].open) apps[a].open();
    }
    raise(a);
}
void wm_close(int a) {
    if (!wins[a].open) return;
    if (apps[a].flush) apps[a].flush();
    raise(a);
    nwin--;
    wins[a].open = 0;
    dirty = 1;
}
int wm_active(int a) { return nwin && order[nwin - 1] == a; }
void power(int reboot) {
    for (int a = 0; a < APP_COUNT; a++) if (wins[a].open && apps[a].flush) apps[a].flush();
    ST->rt->ResetSystem(reboot ? 0 : 2, 0, 0, 0);
}
static void geometry(int a, int *x, int *y, int *w, int *h) {
    if (wins[a].max) { *x = 0; *y = 0; *w = SW; *h = SH - DOCK; return; }
    *x = wins[a].x; *y = wins[a].y;
    *w = apps[a].w > SW ? SW : apps[a].w;
    *h = apps[a].h > SH - DOCK ? SH - DOCK : apps[a].h;
}
/* ---- スタートメニュー ---- */
#define MENU_N (APP_COUNT + 2)
#define MENU_W 230
#define MENU_ROW 26
#define START_W 92
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
        text(20, py + 5, i < APP_COUNT ? apps[i].title : i == APP_COUNT ? u"再起動" : u"シャットダウン", 0xFFFFFF);
        if (i < APP_COUNT && menuKey[i]) text(MENU_W - 28, py + 5, menuKey[i], 0xB0B0D0);
    }
}
/* タスクバー上の i 番目の位置 (開いているウィンドウをアプリ順に並べる) */
static int task_width(void) {
    int n = nwin ? nwin : 1, w = (SW - START_W - 130) / n;
    return w > 130 ? 130 : w;
}

static void draw(void) {
    noclip();
    for (int y = 0; y < SH; y++) { /* 壁紙 */
        u32 r = 24 + 90 * y / SH, g = 26 + 40 * y / SH, b = 62 + 100 * y / SH;
        fill(0, y, SW, 1, r << 16 | g << 8 | b);
    }
    text(SW - 120, 16, u"FreeOS Any 1.3", 0xB8B8E0);
    if (!nwin) text(24, 24, u"Esc かスタートボタンでメニューを開きます", 0xFFFFFF);
    for (int i = 0; i < nwin; i++) {
        int a = order[i], x, y, w, h, top = i == nwin - 1;
        geometry(a, &x, &y, &w, &h);
        noclip();
        fill(x + 5, y + 5, w, h, 0x15152A);
        fill(x, y, w, h, top ? 0x7C6CFF : 0x55556A);
        fill(x + 1, y + TITLE, w - 2, h - TITLE - 1, 0xF4F4FA);
        text(x + 8, y + 4, apps[a].title, 0xFFFFFF);
        text(x + w - 18, y + 4, u"×", 0xFFFFFF);
        clip(x + 1, y + TITLE, w - 2, h - TITLE - 1);
        apps[a].draw(x + 1, y + TITLE, w - 2, h - TITLE - 1);
    }
    noclip();
    fill(0, SH - DOCK, SW, DOCK, 0x16161F);
    fill(6, SH - DOCK + 5, START_W - 10, 26, menu ? 0x9585FF : 0x6D5DFC);
    text(18, SH - DOCK + 10, u"スタート", 0xFFFFFF);
    int x = START_W, tw = task_width();
    for (int a = 0; a < APP_COUNT; a++) {
        if (!wins[a].open) continue;
        fill(x, SH - DOCK + 5, tw - 6, 26, nwin && order[nwin - 1] == a ? 0x6D5DFC : 0x3A3A52);
        clip(x, SH - DOCK, tw - 8, DOCK);
        text(x + 8, SH - DOCK + 10, apps[a].title, 0xFFFFFF);
        noclip();
        x += tw;
    }
    Time t;
    if (!ST->rt->GetTime(&t, 0)) {
        c16 s[24] = u"";
        snum(s, t.month, 0, 24); scat(s, u"/", 24); snum(s, t.day, 0, 24); scat(s, u" ", 24);
        snum(s, t.hour, 2, 24); scat(s, u":", 24); snum(s, t.minute, 2, 24);
        text(SW - 100, SH - DOCK + 10, s, 0xFFFFFF);
    }
    if (noteTicks) {
        int w = 24; for (int i = 0; note[i]; i++) w += charw(note[i]);
        fill(SW - w - 12, SH - DOCK - 40, w, 28, 0x101018);
        text(SW - w, SH - DOCK - 34, note, 0xFFFFFF);
    }
    if (menu) menu_draw();
    if (hasPointer) {
        for (int i = 0; i < 14; i++) { fill(mx, my + i, i * 2 / 3 + 2, 1, 0x000000); if (i > 0 && i < 12) fill(mx + 1, my + i, i * 2 / 3, 1, 0xFFFFFF); }
    }
    present();
}

void redraw(void) { draw(); dirty = 1; }
static void press(void) {
    if (menu) {
        int i = (my - menu_y() - 4) / MENU_ROW;
        if (mx < MENU_W && my >= menu_y() + 4 && i < MENU_N) { menu_run(i); return; }
        menu = 0;
        if (my >= SH - DOCK && mx < START_W) return;
    }
    if (my >= SH - DOCK) {
        if (mx < START_W) { menu = 1; msel = 0; return; }
        int x = START_W, tw = task_width();
        for (int a = 0; a < APP_COUNT; a++) { if (!wins[a].open) continue; if (mx >= x && mx < x + tw) { raise(a); return; } x += tw; }
        return;
    }
    for (int i = nwin - 1; i >= 0; i--) {
        int a = order[i], x, y, w, h;
        geometry(a, &x, &y, &w, &h);
        if (mx < x || my < y || mx >= x + w || my >= y + h) continue;
        raise(a);
        if (my < y + TITLE) {
            if (mx >= x + w - 26) wm_close(a);
            else if (!wins[a].max) { drag = a; dragx = mx - x; dragy = my - y; }
        } else if (apps[a].click) apps[a].click(mx - x - 1, my - y - TITLE);
        return;
    }
}
static void pointer_update(int x, int y, int down) {
    x = x < 0 ? 0 : x >= SW ? SW - 1 : x;
    y = y < 0 ? 0 : y >= SH ? SH - 1 : y;
    if (x != mx || y != my || down != mdown) dirty = 1;
    mx = x; my = y;
    if (menu && mx < MENU_W && my >= menu_y() + 4 && my < menu_y() + 4 + MENU_N * MENU_ROW) msel = (my - menu_y() - 4) / MENU_ROW;
    if (down && !mdown) press();
    if (!down) drag = -1;
    if (drag >= 0) {
        int nx = mx - dragx, ny = my - dragy;
        wins[drag].x = nx < 60 - apps[drag].w ? 60 - apps[drag].w : nx > SW - 60 ? SW - 60 : nx;
        wins[drag].y = ny < 0 ? 0 : ny > SH - DOCK - TITLE ? SH - DOCK - TITLE : ny;
    }
    mdown = down;
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
        pointer_update(mx + dx, my + dy, s.left);
    }
    for (int i = 0; i < nabs; i++) {
        AbsState s; AbsMode *m = abso[i]->Mode;
        if (m->maxx <= m->minx || m->maxy <= m->miny || abso[i]->GetState(abso[i], &s)) continue;
        hasPointer = 1;
        pointer_update((int)((s.x - m->minx) * SW / (m->maxx - m->minx)), (int)((s.y - m->miny) * SH / (m->maxy - m->miny)), s.buttons & 1);
    }
}

static void key(Key k) {
    int top = nwin ? order[nwin - 1] : -1;
    dirty = 1;
    if (k.scan == 0x17) { menu ^= 1; msel = 0; return; }                 /* Esc */
    if (menu) {
        if (k.scan == 1) msel = (msel + MENU_N - 1) % MENU_N;
        else if (k.scan == 2) msel = (msel + 1) % MENU_N;
        else if (k.ch == 13) menu_run(msel);
        else if (k.scan) menu = 0;
        if (menu || !k.scan) return;
    }
    if (k.scan == 0x0B) { if (nwin > 1) raise(order[0]); }               /* F1 */
    else if (k.scan == 0x0E) { if (top >= 0) wm_close(top); }            /* F4 */
    else if (k.scan >= 0x0F && k.scan <= 0x13) wm_open(k.scan - 0x0F);   /* F5-F9 */
    else if (k.scan == 0x14) { if (top >= 0) wins[top].max ^= 1; }       /* F10 */
    else if (top >= 0 && apps[top].key) apps[top].key(k);
}

static Status die(const c16 *msg) {
    struct { void *reset; Status (*out)(void *, const c16 *); } *con = ST->conOut;
    con->out(con, msg);
    BS->Stall(10000000);
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
    net_init(image);
    wm_open(APP_TERM);
    if (fs_stat(u"\\README.TXT", 0)) edit_open(u"\\README.TXT");
    if (!fs_ready()) notify(u"保存先がありません。ファイル機能は使えません");

    Event timer;
    BS->CreateEvent(0x80000000, 0, 0, 0, &timer);
    BS->SetTimer(timer, 1, 200000); /* 20ms 周期 */
    int minute = -1, ticks = 0;
    for (;;) {
        u64 which;
        BS->WaitForEvent(1, &timer, &which);
        Key k;
        for (int i = 0; i < 16 && !ST->conIn->ReadKeyStroke(ST->conIn, &k); i++) key(k);
        pointer_poll();
        tablet_poll();
        net_poll();
        for (int i = 0; i < nwin; i++) if (apps[order[i]].tick) apps[order[i]].tick();
        if (noteTicks && !--noteTicks) dirty = 1;
        if (++ticks % 50 == 0) { Time t; if (!ST->rt->GetTime(&t, 0) && t.minute != minute) { minute = t.minute; dirty = 1; } }
        if (dirty) { dirty = 0; draw(); }
    }
}
