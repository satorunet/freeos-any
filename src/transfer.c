/* ファイル転送: 同じ LAN の PC やスマホのブラウザから HOME のファイルを出し入れする */
#include "net.h"

#define SLOTS 4
#define FILEMAX (32 * 1024 * 1024)
#define HDRMAX 4096
typedef struct {
    int sock, state;           /* 0=空き 1=要求受信 2=本体受信 3=送信 4=送り切り待ち */
    char hdr[HDRMAX]; int hlen;
    u8 *data; u64 len, got;
    c16 path[PATHLEN];
    u64 last;
} Conn;
static Conn conn[SLOTS];
static int running, fails, received, sent;
static char pin[8];
static c16 lastMsg[120];
static Entry entries[MAXLIST];

static int clen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void drop(Conn *c) { if (c->data) BS->FreePool(c->data); c->data = 0; c->state = 0; }
static void stop(void) {
    if (!running) return;
    for (int i = 0; i < SLOTS; i++) drop(&conn[i]);
    tcp_unlisten();
    running = 0;
}
static void start(void) {
    u8 r[4];
    if (!fs_ready()) { scopy(lastMsg, u"保存先がありません", 120); return; }
    scopy(lastMsg, u"接続中… (Esc で中止)", 120); redraw();
    if (!net_up()) { scopy(lastMsg, net_error(), 120); return; }
    if (!entropy(r, 4)) { u64 t = net_ms() * 2654435761u; mcopy(r, &t, 4); }
    u32 v = (r[0] | r[1] << 8 | r[2] << 16 | (u32)r[3] << 24) % 1000000;
    for (int i = 5; i >= 0; i--) pin[i] = '0' + v % 10, v /= 10;
    pin[6] = 0;
    tcp_listen(80);
    running = 1; fails = received = sent = 0; lastMsg[0] = 0;
}

/* 応答の組み立て */
static u8 *out; static u64 olen, ocap;
static void o(const char *s) { while (*s && olen < ocap) out[olen++] = *s++; }
static void onum(u64 v) { char t[24]; int i = 23; t[i] = 0; do t[--i] = '0' + v % 10; while (v /= 10); o(t + i); }
static void ohtml(const c16 *s) {
    static u8 u[8];
    for (; *s; s++) {
        if (*s == u'<') o("&lt;"); else if (*s == u'>') o("&gt;"); else if (*s == u'&') o("&amp;"); else if (*s == u'"') o("&quot;"); else if (*s == u'\'') o("&#39;");
        else { int n = utf8_encode(s, 1, u); u[n] = 0; o((char *)u); }
    }
}
static void ourl(const c16 *s) { /* パスを URL 用に変換 */
    static u8 u[8];
    for (; *s; s++) {
        int n = utf8_encode(s, 1, u);
        for (int i = 0; i < n; i++) {
            u8 c = u[i]; if (c == '\\') c = '/';
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '/') { char t[2] = {(char)c, 0}; o(t); }
            else { char t[4] = {'%', "0123456789ABCDEF"[c >> 4], "0123456789ABCDEF"[c & 15], 0}; o(t); }
        }
    }
}
static void reply(Conn *c, const char *status, const char *type, const u8 *body, u64 len, int attach) {
    u8 *d;
    if (BS->AllocatePool(2, len + 512, (void **)&d)) { tcp_abort(c->sock); drop(c); return; }
    out = d; olen = 0; ocap = 512;
    o("HTTP/1.1 "); o(status); o("\r\nContent-Type: "); o(type); o("\r\nContent-Length: "); onum(len);
    if (attach) o("\r\nContent-Disposition: attachment");
    o("\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
    mcopy(d + olen, body, len);
    if (c->data) BS->FreePool(c->data);
    c->data = d; c->len = olen + len; c->got = 0; c->state = 3;
}
static void say(Conn *c, const char *status) { reply(c, status, "text/plain", (const u8 *)status, clen(status), 0); }

static void listing(Conn *c, const c16 *dir) {
    int n = fs_list(dir, entries, MAXLIST);
    if (n < 0) { say(c, "404 Not Found"); return; }
    static u8 html[128 * 1024];
    out = html; olen = 0; ocap = sizeof html;
    o("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><title>FreeOS Any ファイル転送</title>"
      "<body style='font:16px/2 sans-serif;max-width:640px;margin:20px auto;padding:0 12px'><h2>FreeOS Any : HOME"); ohtml(dir);
    o("</h2><p><input type=file id=f multiple> <button onclick='up()'>このフォルダへ送る</button> <span id=s></span></p><ul>");
    if (dir[0]) { o("<li><a href='/"); o(pin); o("/'>一番上へ</a>"); }
    for (int i = 0; i < n; i++) {
        static c16 p[PATHLEN];
        if (!fs_resolve(dir, entries[i].name, p)) continue;
        o("<li><a href='/"); o(pin); o(entries[i].dir ? "/d" : "/f"); ourl(p); o("'>"); ohtml(entries[i].name); o(entries[i].dir ? "/</a>" : "</a> <small>");
        if (!entries[i].dir) { onum(entries[i].size); o(" B</small>"); }
    }
    o("</ul><script>async function up(){for(const x of f.files){s.textContent='送信中 '+x.name;const r=await fetch('/"); o(pin); o("/f"); ourl(dir);
    o("/'+encodeURIComponent(x.name),{method:'PUT',body:x}).catch(()=>0);if(!r||!r.ok){s.textContent='失敗: '+x.name+' (32MBまで。使えない文字がないか確認)';return}}location.reload()}</script>");
    reply(c, "200 OK", "text/html; charset=utf-8", html, olen, 0);
}
static int unescape(const char *s, int n, c16 *dst, int cap) {
    static u8 b[PATHLEN * 4]; int m = 0;
    for (int i = 0; i < n; i++) {
        int c = (u8)s[i];
        if (c == '%') {
            if (i + 2 >= n) return 0;
            c = 0;
            for (int k = 1; k <= 2; k++) { char h = s[i + k] | 32; int v = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : -1; if (v < 0) return 0; c = c * 16 + v; }
            i += 2;
        }
        if (m >= (int)sizeof b) return 0;
        b[m++] = c;
    }
    int k = utf8_decode(b, m, dst, cap - 1);
    if (k < 0) return 0;
    dst[k] = 0;
    return 1;
}
static void unique(c16 *path) { /* 上書きせず、別の名前にする */
    static c16 p[PATHLEN];
    if (!fs_stat(path, 0)) return;
    int slash = 0;
    for (int i = 0; path[i]; i++) if (path[i] == u'\\') slash = i;
    for (int i = 2; i < 1000; i++) {
        scopy(p, path, slash + 2);
        snum(p, i, 0, PATHLEN); scat(p, u"_", PATHLEN); scat(p, path + slash + 1, PATHLEN);
        if (!fs_stat(p, 0)) { scopy(path, p, PATHLEN); return; }
    }
}
static void finish_upload(Conn *c) {
    unique(c->path);
    if (fs_write(c->path, c->data, c->len)) {
        received++; scopy(lastMsg, u"受け取りました: ", 120); scat(lastMsg, c->path + 1, 120);
        say(c, "200 OK");
    } else say(c, "500 Save Failed");
    invalidate();
}
/* 受信したヘッダを解釈する。body はヘッダの後ろに続いて届いた分 */
static void request(Conn *c, int body) {
    char *h = c->hdr;
    int put = h[0] == 'P' && h[1] == 'U' && h[2] == 'T' && h[3] == ' ';
    int get = h[0] == 'G' && h[1] == 'E' && h[2] == 'T' && h[3] == ' ';
    if (!put && !get) { say(c, "405 Method Not Allowed"); return; }
    char *p = h + 4; int n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '?' && p[n] != '\r') n++;
    int ok = n >= 8 && p[0] == '/' && p[7] == '/';
    for (int i = 0; ok && i < 6; i++) ok = p[1 + i] == pin[i];
    if (!ok) { /* 暗証番号の総当たりを防ぐ */
        if (!(n == 12 && p[1] == 'f') && ++fails >= 20) { stop(); scopy(lastMsg, u"番号の間違いが続いたため停止しました", 120); invalidate(); return; }
        say(c, "404 Not Found"); return;
    }
    p += 8; n -= 8;
    if (!n && get) { listing(c, u""); return; }
    static c16 arg[PATHLEN];
    int isdir = n && p[0] == 'd', isfile = n && p[0] == 'f';
    if ((!isdir && !isfile) || n < 2 || p[1] != '/' || !unescape(p + 1, n - 1, arg, PATHLEN) || !fs_resolve(u"", arg, c->path)) { say(c, "400 Bad Request"); return; }
    if (get && isdir) { listing(c, c->path); return; }
    if (get) {
        Entry e;
        if (!fs_stat(c->path, &e) || e.dir || e.size > FILEMAX) { say(c, "404 Not Found"); return; }
        u8 *d;
        if (BS->AllocatePool(2, e.size + 1, (void **)&d)) { say(c, "503 Busy"); return; }
        i64 got = fs_read(c->path, d, e.size);
        if (got < 0) say(c, "500 Read Failed");
        else { reply(c, "200 OK", "application/octet-stream", d, got, 1); sent++; scopy(lastMsg, u"送りました: ", 120); scat(lastMsg, c->path + 1, 120); invalidate(); }
        BS->FreePool(d);
        return;
    }
    if (!isfile || !c->path[0]) { say(c, "400 Bad Request"); return; }
    i64 want = -1;
    for (char *q = h; *q; q++) {
        const char *k = "\ncontent-length:"; int i = 0;
        while (k[i] && (q[i] | (q[i] >= 'A' && q[i] <= 'Z' ? 32 : 0)) == k[i]) i++;
        if (k[i]) continue;
        q += i; while (*q == ' ') q++;
        for (want = 0; *q >= '0' && *q <= '9' && want <= FILEMAX; q++) want = want * 10 + *q - '0';
        break;
    }
    if (want < 0 || want > FILEMAX) { say(c, "413 Too Large"); return; }
    if (BS->AllocatePool(2, want + 1, (void **)&c->data)) { c->data = 0; say(c, "503 Busy"); return; }
    c->len = want; c->got = c->hlen - body; if (c->got > c->len) c->got = c->len;
    mcopy(c->data, h + body, c->got);
    c->state = 2;
    if (c->got == c->len) finish_upload(c);
}
static void serve(Conn *c) {
    if (c->state == 1) {
        int n = tcp_recv(c->sock, c->hdr + c->hlen, HDRMAX - 1 - c->hlen);
        if (n < 0) { tcp_abort(c->sock); drop(c); return; }
        if (!n) return;
        c->hlen += n; c->hdr[c->hlen] = 0; c->last = net_ms();
        for (int i = 0; i + 3 < c->hlen; i++) if (c->hdr[i] == '\r' && c->hdr[i + 1] == '\n' && c->hdr[i + 2] == '\r' && c->hdr[i + 3] == '\n') { request(c, i + 4); return; }
        if (c->hlen >= HDRMAX - 1) say(c, "431 Too Large");
    } else if (c->state == 2) {
        for (int i = 0; i < 16 && c->got < c->len; i++) {
            int n = tcp_recv(c->sock, c->data + c->got, (int)(c->len - c->got));
            if (n < 0) { tcp_abort(c->sock); drop(c); return; }
            if (!n) break;
            c->got += n; c->last = net_ms();
        }
        if (c->got == c->len) finish_upload(c);
    } else if (c->state == 3) {
        int n = tcp_send(c->sock, c->data + c->got, (int)(c->len - c->got > 32768 ? 32768 : c->len - c->got));
        if (n < 0) { tcp_abort(c->sock); drop(c); return; }
        if (n) c->got += n, c->last = net_ms();
        if (c->got == c->len) c->state = 4;
    } else if (c->state == 4 && tcp_sent_all(c->sock)) { tcp_close(c->sock); drop(c); }
}
void transfer_tick(void) {
    if (!running) return;
    for (int round = 0; round < 8 && running; round++) {
        net_poll();
        for (int i = 0; i < SLOTS; i++) if (!conn[i].state) {
            int s = tcp_accept();
            if (s < 0) break;
            conn[i].sock = s; conn[i].state = 1; conn[i].hlen = 0; conn[i].last = net_ms();
        }
        int busy = 0;
        for (int i = 0; i < SLOTS; i++) if (conn[i].state) {
            if (net_ms() - conn[i].last > 15000) { tcp_abort(conn[i].sock); drop(&conn[i]); continue; }
            serve(&conn[i]);
            busy = 1;
            if (!running) return;
        }
        if (!busy) break;
        BS->Stall(500);
    }
}
void transfer_key(Key k) { if (k.ch == 13) { if (running) stop(); else start(); } }
void transfer_click(int x, int y) { if (y >= 12 && y < 44 && x >= 16 && x < 200) { Key k = {0, 13}; transfer_key(k); } }
void transfer_stop(void) { stop(); }
void transfer_draw(int x, int y, int w, int h) {
    (void)w; (void)h;
    fill(x + 16, y + 12, 184, 30, running ? 0xE5484D : 0x6D5DFC);
    text(x + 28, y + 19, running ? u"停止する (Enter)" : u"開始する (Enter)", 0xFFFFFF);
    if (running) {
        c16 url[64] = u"http://", ip[24];
        net_ipstr(net_ip(), ip, 24); scat(url, ip, 64); scat(url, u"/", 64);
        int n = slen(url); for (int i = 0; i < 6; i++) url[n++] = pin[i];
        url[n++] = u'/'; url[n] = 0;
        text(x + 16, y + 60, u"同じネットワークのPCやスマホのブラウザで開く:", 0x55556A);
        fill(x + 12, y + 82, 400, 24, 0xFFF4C2);
        text(x + 18, y + 86, url, 0x1D1D27);
        c16 s[64] = u"受信 "; snum(s, received, 0, 64); scat(s, u" 件 / 送信 ", 64); snum(s, sent, 0, 64); scat(s, u" 件", 64);
        text(x + 16, y + 118, s, 0x1D1D27);
        text(x + 16, y + 144, u"通信は暗号化されません。信頼できるLANで使い、", 0xA04000);
        text(x + 16, y + 162, u"終わったら停止してください。", 0xA04000);
    } else text(x + 16, y + 60, u"HOME のファイルをやり取りできます (有線LAN)", 0x55556A);
    text(x + 16, y + 192, lastMsg, 0x5A46E0);
}
