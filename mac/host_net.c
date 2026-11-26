#define HOST_LIBC
/* Mac アプリ版の通信: net.h の関数を macOS の URLSession で用意する (TCP/IP と TLS は macOS が担当) */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include "../src/net.h"
#include "host.h"

u64 net_ms(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom / 1000000; }
void net_init(Handle image) { (void)image; }
int net_up(void) { return 1; }
int net_ready(void) { return 1; }
static c16 err[160];
const c16 *net_error(void) { return err; }
void net_set_error(const c16 *m) { int i = 0; for (; m[i] && i < 159; i++) err[i] = m[i]; err[i] = 0; }
static void set_err8(const char *m) { /* UTF-8 → UTF-16 */
    int k = 0; const unsigned char *p = (const unsigned char *)m;
    while (*p && k < 159) { unsigned c = *p++; if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; } else if (c >= 0xC0) { c = (c & 31) << 6 | (p[0] & 63); p++; } err[k++] = (c16)c; }
    err[k] = 0;
}
u32 net_ip(void) { return 0; }
void net_ipstr(u32 ip, c16 *out, int cap) { (void)ip; const char *s = "(Mac の通信を使用)"; (void)s; if (cap > 0) { const c16 m[] = u"Mac の通信を使用"; int i = 0; for (; m[i] && i < cap - 1; i++) out[i] = m[i]; out[i] = 0; } }
void net_poll(void) {}
int ui_pump(void);
int net_wait(void) { if (!ui_pump()) { net_set_error(u"中止しました"); return 0; } usleep(5000); return 1; }
int net_resolve(const char *host, u32 *ip) { (void)host; *ip = 0; return 1; }
/* ファイル転送 (サーバー) は Mac 版では使えない */
int tcp_connect(u32 ip, u16 port) { (void)ip; (void)port; return -1; }
int tcp_listen(u16 port) { (void)port; net_set_error(u"Mac アプリ版ではファイル転送は使えません"); return -1; }
void tcp_unlisten(void) {}
int tcp_accept(void) { return -1; }
int tcp_send(int s, const void *b, int l) { (void)s; (void)b; (void)l; return -1; }
int tcp_recv(int s, void *b, int c) { (void)s; (void)b; (void)c; return -1; }
int tcp_sent_all(int s) { (void)s; return 1; }
void tcp_close(int s) { (void)s; }
void tcp_abort(int s) { (void)s; }
int entropy(u8 *out, int n) { arc4random_buf(out, (size_t)n); return 1; }

static int split(const char *url, char *host, const char **path, int *https) {
    *https = !strncmp(url, "https:", 6);
    const char *p = strstr(url, "//"); if (!p) return 0; p += 2;
    int k = 0; while (*p && *p != '/' && *p != ':' && *p != '?' && k < 255) host[k++] = *p++;
    host[k] = 0; while (*p && *p != '/' && *p != '?') p++;
    *path = *p ? p : "/"; return 1;
}
int http_get(const char *url, u8 *buf, u64 cap, HttpResult *r) {
    char host[256]; const char *path; int https;
    memset(r, 0, sizeof *r);
    if (!split(url, host, &path, &https)) { net_set_error(u"URL が正しくありません"); return 0; }
    static char ck[2048]; ck[0] = 0; cookie_header(host, path, https, ck, sizeof ck, 0);
    int id = host_http_start(url, ck);
    if (id < 0) { net_set_error(u"通信を始められません"); return 0; }
    static char setc[16384], e8[256];
    for (;;) {
        int st = host_http_poll(id, &r->status, r->type, sizeof r->type, r->url, URLLEN, setc, sizeof setc, buf, cap, &r->len, &r->truncated, e8, sizeof e8);
        if (st < 0) { set_err8(e8); return 0; }
        if (st > 0) break;
        if (!net_wait()) { host_http_cancel(id); return 0; }
    }
    if (!r->url[0]) { strncpy(r->url, url, URLLEN - 1); }
    char fh[256]; const char *fp; int fs;
    if (split(r->url, fh, &fp, &fs)) for (char *l = setc; *l;) { char *e = strchr(l, '\n'); if (e) *e = 0; if (*l) cookie_from_response(fh, fp, l); if (!e) break; l = e + 1; }
    return 1;
}
