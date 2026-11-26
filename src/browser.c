/* ブラウザ: HTML を文章とリンクにして表示する (画像・CSS・JavaScript は非対応) */
#include "net.h"

#define DOCMAX (200 * 1024)
#define PAGEMAX (4 * 1024 * 1024)
#define SAVEMAX (16 * 1024 * 1024)
#define LINKMAX 1500
#define LINEMAX 12000

static c16 *doc; static u16 *lk;      /* 文字と、その文字が属するリンク番号 (0=なし) */
static int *lines;
static char (*links)[URLLEN];
static u8 *page, *raw;
static int dlen, nlink, nline, top, sel, laidW, viewRows, focus = 1;
static char addr[URLLEN], current[URLLEN], history[16][URLLEN];
static int nhist;
static u64 pageLen;
static c16 status[160], title[80];

static int clen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void ccopy(char *d, const char *s, int cap) { int n = 0; while (s[n] && n < cap - 1) { d[n] = s[n]; n++; } d[n] = 0; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int prefix(const char *s, const char *p) { while (*p) if (lower(*s++) != *p++) return 0; return 1; }

/* 相対 URL を絶対 URL にする */
void url_join(const char *base, const char *ref, char *out) {
    static char t[URLLEN * 2];
    int n = 0;
    const char *c = ref;
    while ((lower(*c) >= 'a' && lower(*c) <= 'z') || *c == '+') c++;
    if (*c == ':' && c != ref) { ccopy(out, ref, URLLEN); return; }
    const char *host = base;
    while (*host && !(host[0] == '/' && host[1] == '/')) host++;
    if (*host) host += 2;
    const char *path = host;
    while (*path && *path != '/' && *path != '?' && *path != '#') path++;
    const char *end = path;
    if (ref[0] == '/' && ref[1] == '/') end = host - 2;
    else if (ref[0] == '#') { while (*end && *end != '#') end++; }
    else if (ref[0] == '?') { while (*end && *end != '?' && *end != '#') end++; }
    else if (ref[0] != '/') { /* 今の場所のフォルダから */
        const char *q = path;
        while (*q && *q != '?' && *q != '#') { if (*q == '/') end = q + 1; q++; }
    }
    for (const char *p = base; p < end && n < URLLEN - 1; p++) t[n++] = *p;
    if (ref[0] != '/' && ref[0] != '?' && ref[0] != '#' && (!n || t[n - 1] != '/') && end == path) t[n++] = '/';
    for (const char *p = ref; *p && n < URLLEN * 2 - 1; p++) t[n++] = *p;
    t[n] = 0;
    /* パス中の ./ と ../ を整理する */
    int start = (int)(path - base), w = start < n ? start : n, r = w;
    while (r < n && t[r] != '?' && t[r] != '#') {
        if (t[r] == '/' && t[r + 1] == '.' && (t[r + 2] == '/' || !t[r + 2])) { r += 2; if (!t[r]) t[w++] = '/'; }
        else if (t[r] == '/' && t[r + 1] == '.' && t[r + 2] == '.' && (t[r + 3] == '/' || !t[r + 3])) {
            r += 3; while (w > start && t[--w] != '/') {}
            if (r >= n) t[w++] = '/';
        } else t[w++] = t[r++];
    }
    while (r < n) t[w++] = t[r++];
    t[w] = 0;
    ccopy(out, t, URLLEN);
}

static void add(c16 c, int link) { if (dlen < DOCMAX) { doc[dlen] = c; lk[dlen] = link; dlen++; } }
static void brk(int blank) { /* 改行。blank なら空行まで */
    while (dlen && doc[dlen - 1] == u' ') dlen--;
    if (!dlen) return;
    if (doc[dlen - 1] != u'\n') add(u'\n', 0);
    if (blank && dlen > 1 && doc[dlen - 2] != u'\n') add(u'\n', 0);
}
static u32 next_char(const u8 *s, u64 n, u64 *i) { /* UTF-8 を1文字 */
    u32 c = s[(*i)++]; int more = 0;
    if (c >= 0xF0) c &= 7, more = 3; else if (c >= 0xE0) c &= 15, more = 2; else if (c >= 0xC0) c &= 31, more = 1; else if (c >= 0x80) return u'?';
    while (more-- && *i < n && (s[*i] & 0xC0) == 0x80) c = c << 6 | (s[(*i)++] & 63);
    return c > 0xFFFF ? u'□' : c;
}
static u32 entity(const u8 *s, u64 n, u64 *i) { /* *i は & の次 */
    static const struct { const char *name; u16 c; } e[] = {{"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''}, {"nbsp;", ' '},
        {"copy;", 0xA9}, {"reg;", 0xAE}, {"middot;", 0xB7}, {"times;", 0xD7}, {"raquo;", 0xBB}, {"laquo;", 0xAB}, {"hellip;", 0x2026}, {"mdash;", 0x2015}, {"ndash;", '-'}, {"rarr;", 0x2192}, {"larr;", 0x2190}};
    u64 p = *i;
    if (p < n && s[p] == '#') {
        int hex = p + 1 < n && lower(s[p + 1]) == 'x'; u32 v = 0; int d = 0;
        for (p += 1 + hex; p < n; p++, d++) {
            char c = lower(s[p]); int x = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (x < 0 || d > 7) break;
            v = v * (hex ? 16 : 10) + x;
        }
        if (d) { *i = p < n && s[p] == ';' ? p + 1 : p; return v > 0xFFFF || v < 32 ? u' ' : v; }
    } else for (unsigned k = 0; k < sizeof e / sizeof *e; k++) {
        u64 q = p; const char *m = e[k].name;
        while (*m && q < n && s[q] == (u8)*m) q++, m++;
        if (!*m) { *i = q; return e[k].c; }
    }
    return u'&';
}
/* タグの中から属性を取り出す */
static int attribute(const u8 *t, int n, const char *name, char *out, int cap) {
    int i = 0;
    while (i < n && t[i] > 32) i++;
    while (i < n) {
        while (i < n && (t[i] <= 32 || t[i] == '/')) i++;
        int s = i;
        while (i < n && t[i] > 32 && t[i] != '=' && t[i] != '/') i++;
        int match = i - s == clen(name) && prefix((const char *)t + s, name);
        while (i < n && t[i] <= 32) i++;
        int vs = i, ve = i;
        if (i < n && t[i] == '=') {
            i++;
            while (i < n && t[i] <= 32) i++;
            if (i < n && (t[i] == '"' || t[i] == '\'')) { u8 q = t[i++]; vs = i; while (i < n && t[i] != q) i++; ve = i; i++; }
            else { vs = i; while (i < n && t[i] > 32) i++; ve = i; }
        }
        if (match) {
            int m = 0;
            for (u64 p = vs; p < (u64)ve && m < cap - 1;) {
                u8 c = t[p++];
                if (c == '&') { u32 v = entity(t, ve, &p); c = v < 128 ? (u8)v : '&'; }
                if (c >= 32) out[m++] = c;
            }
            out[m] = 0;
            return 1;
        }
        if (i == s) i++;
    }
    return 0;
}
static int tag_is(const char *tag, const char *list) { /* list は " p div br " の形 */
    int n = clen(tag);
    for (const char *p = list; *p; p++) if (*p == ' ') { int i = 0; while (i < n && p[1 + i] == tag[i]) i++; if (i == n && p[1 + n] == ' ') return 1; }
    return 0;
}
static void render(const u8 *s, u64 n, int html) {
    dlen = nlink = 0; title[0] = 0;
    int link = 0, pre = 0, intitle = 0, tlen = 0;
    for (u64 i = 0; i < n && dlen < DOCMAX;) {
        if (html && s[i] == '<' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '!' || s[i + 1] == '?' || (lower(s[i + 1]) >= 'a' && lower(s[i + 1]) <= 'z'))) {
            if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
                for (i += 4; i + 2 < n && !(s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>'); i++) {}
                i += 3; continue;
            }
            u64 st = ++i; u8 q = 0;
            for (; i < n; i++) {
                if (q) { if (s[i] == q) q = 0; }
                else if ((s[i] == '"' || s[i] == '\'') && i > st && s[i - 1] == '=') q = s[i];
                else if (s[i] == '>') break;
            }
            int tn = (int)(i - st); i++;
            int close = tn && s[st] == '/';
            char tag[12]; int m = 0;
            for (int k = close; k < tn && m < 11 && s[st + k] > 32 && s[st + k] != '/'; k++) tag[m++] = lower(s[st + k]);
            tag[m] = 0;
            if (!close && tag_is(tag, " script style svg template iframe ")) { /* 中身ごと飛ばす */
                for (; i + m + 1 < n; i++) if (s[i] == '<' && s[i + 1] == '/' && prefix((const char *)s + i + 2, tag)) break;
                continue;
            }
            if (tag_is(tag, " title ")) intitle = !close;
            else if (tag_is(tag, " a ")) {
                static char href[URLLEN];
                link = 0;
                if (!close && nlink < LINKMAX - 1 && attribute(s + st, tn, "href", href, URLLEN) && href[0] != '#' && !prefix(href, "javascript:") && !prefix(href, "mailto:")) {
                    link = ++nlink;
                    url_join(current, href, links[link]);
                }
            } else if (tag_is(tag, " img ") && !close) {
                char alt[120];
                if (attribute(s + st, tn, "alt", alt, sizeof alt) && alt[0]) {
                    add(u'[', link);
                    for (u64 p = 0, l = clen(alt); p < l;) add(next_char((u8 *)alt, l, &p), link);
                    add(u']', link);
                }
            } else if (tag_is(tag, " br ")) add(u'\n', 0);
            else if (tag_is(tag, " p h1 h2 h3 h4 h5 h6 table ul ol blockquote article section ")) brk(1);
            else if (tag_is(tag, " div tr header footer nav form dt dd option main figure ")) brk(0);
            else if (tag_is(tag, " li ")) { brk(0); if (!close) add(u'・', 0); }
            else if (tag_is(tag, " td th ") && close) add(u' ', 0);
            else if (tag_is(tag, " hr ")) { brk(0); for (int k = 0; k < 40; k++) add(u'─', 0); brk(0); }
            else if (tag_is(tag, " pre ")) { brk(0); pre = !close; }
            continue;
        }
        u32 c;
        if (html && s[i] == '&') { i++; c = entity(s, n, &i); } else c = next_char(s, n, &i);
        if (c == '\r' || c == 0xFEFF) continue;
        if (!pre && html && (c == '\n' || c == '\t' || c == ' ')) {
            if (intitle) { if (tlen && tlen < 79) title[tlen++] = u' '; }
            else if (dlen && doc[dlen - 1] != u' ' && doc[dlen - 1] != u'\n') add(u' ', link);
            continue;
        }
        if (intitle) { if (tlen < 79) title[tlen++] = c; continue; }
        if (c == '\t') c = ' ';
        add(c < 32 && c != '\n' ? u'?' : c, link);
    }
    title[tlen] = 0;
    top = 0; sel = 0; laidW = 0;
}

static const char home[] =
    "<h2>FreeOS Any ブラウザ</h2><p>上の欄にURLを入力して Enter。有線LANが必要です。</p>"
    "<ul><li><a href='https://example.com/'>example.com</a></li><li><a href='https://ja.wikipedia.org/wiki/オペレーティングシステム'>Wikipedia: OS</a></li>"
    "<li><a href='https://lite.duckduckgo.com/lite/'>DuckDuckGo (軽量版)</a></li></ul>"
    "<p>操作: Tab でリンク選択、Enter で開く、矢印とPageUp/Downで移動、BackSpace で戻る、F2 でURL入力、s でページを保存。</p>"
    "<p>文章とリンクだけを表示します。画像、JavaScript、古い文字コード(Shift_JISなど)は未対応です。</p>";

static int init(void) {
    if (lines) return 1;
    if (BS->AllocatePool(2, DOCMAX * 2, (void **)&doc) || BS->AllocatePool(2, DOCMAX * 2, (void **)&lk) ||
        BS->AllocatePool(2, (u64)LINKMAX * URLLEN, (void **)&links) || BS->AllocatePool(2, PAGEMAX, (void **)&page) ||
        BS->AllocatePool(2, LINEMAX * sizeof(int), (void **)&lines)) { lines = 0; return 0; }
    return 1;
}
static void show_home(void) { current[0] = 0; addr[0] = 0; pageLen = 0; render((const u8 *)home, sizeof home - 1, 1); scopy(status, u"", 160); focus = 1; }

static void load(const char *url, int remember) {
    static char want[URLLEN], prev[URLLEN];
    if (!init()) return;
    want[0] = 0;
    if (!prefix(url, "http://") && !prefix(url, "https://")) ccopy(want, "https://", URLLEN);
    ccopy(want + clen(want), url, URLLEN - clen(want));
    ccopy(prev, current, URLLEN);
    ccopy(addr, want, URLLEN);
    scopy(status, u"読み込み中… (Esc で中止)", 160);
    focus = 0;
    redraw();
    HttpResult r;
    pageLen = 0;
    if (!http_get(want, page, PAGEMAX, &r)) { scopy(status, net_error(), 160); invalidate(); return; }
    if (remember && prev[0]) {
        if (nhist == 16) { mcopy(history[0], history[1], 15 * URLLEN); nhist--; }
        ccopy(history[nhist++], prev, URLLEN);
    }
    ccopy(current, r.url, URLLEN); ccopy(addr, r.url, URLLEN);
    pageLen = r.len;
    char type[80]; int i = 0;
    for (; r.type[i]; i++) type[i] = lower(r.type[i]);
    type[i] = 0;
    int html = !type[0], text = 0, sjis = 0;
    for (char *p = type; *p; p++) { if (prefix(p, "html")) html = 1; if (prefix(p, "text/") || prefix(p, "json") || prefix(p, "xml")) text = 1; if (prefix(p, "shift_jis") || prefix(p, "euc-jp")) sjis = 1; }
    if (html || text) render(page, r.len, html);
    else { static const char m[] = "<p>このファイルは表示できません。s キーで保存できます。</p>"; render((const u8 *)m, sizeof m - 1, 1); }
    status[0] = 0;
    if (r.status != 200) { scat(status, u"HTTP ", 160); snum(status, r.status, 0, 160); scat(status, u"  ", 160); }
    if (sjis) scat(status, u"未対応の文字コードのため文字化けします  ", 160);
    if (r.truncated) scat(status, u"⚠ 途中で切れています", 160);
    if (!status[0]) scopy(status, title, 160);
    invalidate();
}
void browser_go(const char *url) { wm_open(APP_BROWSER); load(url, 1); }

/* URL の最後の部分からファイル名を作って保存先を決める */
static void save_name(const char *url, c16 *path) {
    c16 name[40]; int n = 0;
    const char *s = url, *end = url;
    while (*end && *end != '?' && *end != '#') end++;
    for (const char *p = url; p < end; p++) if (*p == '/' && p + 1 < end) s = p + 1;
    for (; s < end && n < 32; s++) {
        char c = *s;
        if (c == '/') break;
        name[n++] = (lower(c) >= 'a' && lower(c) <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ? c : '_';
    }
    while (n && name[n - 1] == u'.') n--;
    name[n] = 0;
    if (!n || name[0] == u'.') scopy(name, u"download.html", 40);
    for (int i = 1; i < 1000; i++) {
        scopy(path, u"\\", PATHLEN);
        if (i > 1) { snum(path, i, 0, PATHLEN); scat(path, u"_", PATHLEN); }
        scat(path, name, PATHLEN);
        if (!fs_stat(path, 0)) return;
    }
}
/* URL を取得してファイルに保存する。path が空なら名前を自動で付ける */
int download(const char *url, c16 *path) {
    static char want[URLLEN];
    want[0] = 0;
    if (!prefix(url, "http://") && !prefix(url, "https://")) ccopy(want, "https://", URLLEN);
    ccopy(want + clen(want), url, URLLEN - clen(want));
    if (!raw && BS->AllocatePool(2, SAVEMAX, (void **)&raw)) { net_set_error(u"メモリが足りません"); return 0; }
    HttpResult r;
    if (!http_get(want, raw, SAVEMAX, &r)) return 0;
    if (r.status != 200) { net_set_error(u"サーバーがエラーを返しました"); return 0; }
    if (r.truncated) { net_set_error(u"途中で切れたため保存しません (16MBまで)"); return 0; }
    if (!path[0]) save_name(r.url, path);
    if (!fs_write(path, raw, r.len)) { net_set_error(u"保存できません"); return 0; }
    return 1;
}

static void layout(int w) {
    if (w == laidW) return;
    laidW = w; nline = 0;
    int px = 0;
    lines[nline++] = 0;
    for (int i = 0; i < dlen && nline < LINEMAX; i++) {
        if (doc[i] == u'\n') { lines[nline++] = i + 1; px = 0; continue; }
        int cw = charw(doc[i]);
        if (px + cw > w) { lines[nline++] = i; px = 0; }
        px += cw;
    }
}
static void scroll_to_link(void) {
    for (int l = 0; l < nline; l++) {
        int end = l + 1 < nline ? lines[l + 1] : dlen;
        for (int i = lines[l]; i < end; i++) if (lk[i] == sel) { if (l < top || l >= top + viewRows) top = l > 2 ? l - 2 : 0; return; }
    }
}
static void follow(int n) { static char u[URLLEN]; ccopy(u, links[n], URLLEN); load(u, 1); }
static void back(void) {
    if (!nhist) { show_home(); return; }
    static char u[URLLEN]; ccopy(u, history[--nhist], URLLEN); load(u, 0);
}
void browser_open(void) { if (init() && !current[0]) show_home(); }
void browser_key(Key k) {
    if (!lines) return;
    if (k.scan == 0x0C) { focus = 1; return; } /* F2 */
    int n = clen(addr);
    if (focus) {
        if (k.ch == 13) { if (n) load(addr, 1); }
        else if (k.ch == 8) { if (n) addr[n - 1] = 0; }
        else if (k.scan == 8) addr[0] = 0;
        else if (k.ch == 9 || k.scan == 2) focus = 0;
        else if (k.ch > 32 && k.ch < 127 && n < URLLEN - 1) addr[n] = k.ch, addr[n + 1] = 0;
        return;
    }
    if (k.scan == 1) top--;
    else if (k.scan == 2) top++;
    else if (k.scan == 9) top -= viewRows - 1;
    else if (k.scan == 10 || k.ch == u' ') top += viewRows - 1;
    else if (k.scan == 5) top = 0;
    else if (k.scan == 6) top = nline;
    else if (k.ch == 9 && nlink) { sel = sel % nlink + 1; scroll_to_link(); }
    else if (k.ch == 13 && sel) follow(sel);
    else if (k.ch == 8) back();
    else if (k.ch == u's' && current[0] && pageLen) {
        c16 path[PATHLEN]; path[0] = 0;
        save_name(current, path);
        /* page は表示用に取得したままの内容 */
        if (fs_write(path, page, pageLen)) { scopy(status, u"保存しました: ", 160); scat(status, path + 1, 160); }
        else scopy(status, u"保存できません", 160);
    }
}
void browser_draw(int x, int y, int w, int h) {
    fill(x, y, w, h, 0xFFFFFF);
    if (!lines) { text(x + 8, y + 8, u"メモリが足りません", 0); return; }
    fill(x, y, w, 30, 0xE4E4F2);
    text(x + 10, y + 7, u"←", 0x33334A);
    fill(x + 30, y + 4, w - 36, 22, focus ? 0xFFFFFF : 0xF4F4FA);
    if (focus) fill(x + 30, y + 25, w - 36, 2, 0x6D5DFC);
    int n = clen(addr), fit = (w - 56) / 8, from = n > fit ? n - fit : 0, px = x + 36;
    for (int i = from; i < n; i++) px += glyph(px, y + 7, (u8)addr[i], 0x1D1D27);
    if (focus) fill(px + 1, y + 7, 2, 16, 0x6D5DFC);
    int width = w - 20;
    layout(width);
    viewRows = (h - 30 - 24) / 18;
    if (top > nline - viewRows) top = nline - viewRows;
    if (top < 0) top = 0;
    for (int l = top; l < nline && l < top + viewRows; l++) {
        int end = l + 1 < nline ? lines[l + 1] : dlen, cx = x + 10, cy = y + 34 + (l - top) * 18;
        for (int i = lines[l]; i < end; i++) {
            if (doc[i] == u'\n') continue;
            int cw = charw(doc[i]), on = lk[i] && lk[i] == sel;
            if (on) fill(cx, cy, cw, 17, 0x6D5DFC);
            glyph(cx, cy, doc[i], on ? 0xFFFFFF : lk[i] ? 0x2455C8 : 0x1D1D27);
            if (lk[i]) fill(cx, cy + 16, cw, 1, 0x2455C8);
            cx += cw;
        }
    }
    fill(x, y + h - 22, w, 22, 0xE4E4F2);
    text(x + 8, y + h - 19, sel && !focus ? u"" : status, 0x55556A);
    if (sel && !focus) { int cx = x + 8; for (int i = 0; links[sel][i] && cx < x + w - 16; i++) cx += glyph(cx, y + h - 19, (u8)links[sel][i], 0x2455C8); }
}
void browser_click(int x, int y) {
    if (!lines) return;
    if (y < 30) { if (x < 30) back(); else focus = 1; return; }
    focus = 0;
    int l = top + (y - 34) / 18;
    if (y < 34 || l >= nline || l >= top + viewRows) return;
    int end = l + 1 < nline ? lines[l + 1] : dlen, cx = 10;
    for (int i = lines[l]; i < end; i++) {
        if (doc[i] == u'\n') continue;
        int cw = charw(doc[i]);
        if (x >= cx && x < cx + cw) { if (lk[i]) follow(lk[i]); return; }
        cx += cw;
    }
}
