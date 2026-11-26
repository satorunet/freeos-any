/* HTTP と HTTPS (TLS は BearSSL, 証明書はビルド時に取り込んだルート証明書で検証) */
#include "net.h"
#include "bearssl.h"
#include "../third_party/trust_anchors.h"

static int clen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void cadd(char *d, const char *s, int cap) { int n = clen(d); while (*s && n < cap - 1) d[n++] = *s++; d[n] = 0; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int prefix(const char *s, const char *p) { while (*p) if (lower(*s++) != *p++) return 0; return 1; }

static int sockfd;
static u64 deadline;
static int wait_more(void) {
    if (net_ms() > deadline) { net_set_error(u"応答がありません"); return 0; }
    return net_wait();
}
static int low_read(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    for (;;) {
        int n = tcp_recv(sockfd, buf, (int)len);
        if (n > 0) { deadline = net_ms() + 20000; return n; }
        if (n < 0 || !wait_more()) return -1;
    }
}
static int low_write(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    for (;;) {
        int n = tcp_send(sockfd, buf, (int)len);
        if (n > 0) return n;
        if (n < 0 || !wait_more()) return -1;
    }
}

static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static br_sslio_context io;
static unsigned char tlsbuf[BR_SSL_BUFSIZE_BIDI];

static int tls_start(const char *host) {
    u8 seed[48]; Time t;
    if (!entropy(seed, sizeof seed)) { net_set_error(u"安全な乱数が使えないPCのため HTTPS は使えません"); return 0; }
    if (ST->rt->GetTime(&t, 0) || t.year < 2025) { net_set_error(u"PCの時計がずれているため HTTPS を確認できません"); return 0; }
    br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
    /* 1970-01-01 からの日数を求め、BearSSL の形式 (西暦0年起点) にする */
    i64 y = t.year - (t.month <= 2), era = y / 400, yoe = y - era * 400;
    i64 doy = (153 * (t.month + (t.month > 2 ? -3 : 9)) + 2) / 5 + t.day - 1;
    i64 days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    br_x509_minimal_set_time(&xc, (u32)(days + 719528), t.hour * 3600 + t.minute * 60 + t.second);
    br_ssl_engine_set_buffer(&sc.eng, tlsbuf, sizeof tlsbuf, 1);
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof seed);
    if (!br_ssl_client_reset(&sc, host, 0)) { net_set_error(u"暗号化を開始できません"); return 0; }
    br_sslio_init(&io, &sc.eng, low_read, 0, low_write, 0);
    return 1;
}
static void tls_error(void) {
    static c16 msg[80];
    int e = br_ssl_engine_last_error(&sc.eng);
    if (e == BR_ERR_X509_EXPIRED) net_set_error(u"サイトの証明書が期限切れか、PCの時計がずれています");
    else if (e == BR_ERR_X509_BAD_SERVER_NAME) net_set_error(u"証明書のサイト名が違います (接続を中止)");
    else if (e == BR_ERR_X509_NOT_TRUSTED) net_set_error(u"信頼できない証明書です (接続を中止)");
    else { scopy(msg, u"HTTPS 通信エラー ", 80); snum(msg, e, 0, 80); net_set_error(msg); }
}

/* "http(s)://host[:port]/path" を分解する */
static int parse(const char *url, int *https, char *host, int *port, const char **path) {
    if (prefix(url, "https://")) *https = 1, url += 8;
    else if (prefix(url, "http://")) *https = 0, url += 7;
    else return 0;
    int n = 0;
    while (*url && *url != '/' && *url != ':' && *url != '?' && *url != '#') {
        char c = lower(*url++);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_') || n >= 250) return 0;
        host[n++] = c;
    }
    host[n] = 0;
    *port = *https ? 443 : 80;
    if (*url == ':') { int p = 0; for (url++; *url >= '0' && *url <= '9' && p < 65536; url++) p = p * 10 + *url - '0'; if (!p || p > 65535) return 0; *port = p; }
    *path = url;
    return n > 0;
}
static const char *header(const char *h, const char *end, const char *name, char *out, int cap) {
    for (const char *p = h; p < end;) {
        if (prefix(p, name) && p[clen(name)] == ':') {
            p += clen(name) + 1;
            while (*p == ' ') p++;
            int n = 0;
            while (p < end && *p != '\r' && *p != '\n' && n < cap - 1) out[n++] = *p++;
            out[n] = 0;
            return out;
        }
        while (p < end && *p++ != '\n') {}
    }
    return 0;
}
static int dechunk(u8 *b, u64 n, u64 *out) {
    u64 r = 0, w = 0;
    for (;;) {
        u64 size = 0; int digits = 0;
        for (; r < n; r++, digits++) {
            char c = lower(b[r]); int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (v < 0 || digits > 8) break;
            size = size * 16 + v;
        }
        while (r < n && b[r] != '\n') r++;
        if (r >= n || !digits) { *out = w; return 0; }
        r++;
        if (!size) { *out = w; return 1; }
        u64 take = size > n - r ? n - r : size;
        mcopy(b + w, b + r, take); w += take; r += take;
        if (take < size) { *out = w; return 0; }
        while (r < n && b[r] != '\n') r++;
        r++;
    }
}
static int once(const char *url, u8 *buf, u64 cap, HttpResult *r, char *location) {
    int https, port; static char host[256], req[URLLEN * 3 + 512];
    const char *path;
    if (!parse(url, &https, host, &port, &path)) { net_set_error(u"URL が正しくありません"); return 0; }
    u32 ip;
    if (!net_up() || !net_resolve(host, &ip)) return 0;
    if ((sockfd = tcp_connect(ip, port)) < 0) return 0;
    deadline = net_ms() + 20000;

    req[0] = 0;
    cadd(req, "GET ", sizeof req);
    if (*path != '/') cadd(req, "/", sizeof req);
    for (const u8 *p = (const u8 *)path; *p && *p != '#'; p++) { /* 空白や日本語は %XX にする */
        char e[4] = {(char)*p, 0};
        if (*p <= 32 || *p >= 127) { e[0] = '%'; e[1] = "0123456789ABCDEF"[*p >> 4]; e[2] = "0123456789ABCDEF"[*p & 15]; }
        cadd(req, e, sizeof req);
    }
    cadd(req, " HTTP/1.1\r\nHost: ", sizeof req); cadd(req, host, sizeof req);
    if (port != (https ? 443 : 80)) {
        char num[8]; int i = 7, v = port; num[i] = 0;
        do num[--i] = '0' + v % 10; while (v /= 10);
        cadd(req, ":", sizeof req); cadd(req, num + i, sizeof req);
    }
    { /* このサイトのクッキーを送る */
        static char ck[2048];
        if (cookie_header(host, *path ? path : "/", https, ck, sizeof ck, 0)) { cadd(req, "\r\nCookie: ", sizeof req); cadd(req, ck, sizeof req); }
    }
    cadd(req, "\r\nUser-Agent: Mozilla/5.0 (compatible; FreeOSAny/2.5)\r\nAccept: text/html,text/plain,*/*\r\n"
              "Accept-Encoding: identity\r\nConnection: close\r\n\r\n", sizeof req);

    u64 n = 0; int ok = 1, clean = 1;
    if (https) {
        if (!tls_start(host)) ok = 0;
        else if (br_sslio_write_all(&io, req, clen(req)) || br_sslio_flush(&io)) { tls_error(); ok = 0; }
        else {
            while (n < cap) {
                int got = br_sslio_read(&io, buf + n, cap - n);
                if (got < 0) break;
                n += got;
            }
            int e = br_ssl_engine_last_error(&sc.eng);
            clean = e == BR_ERR_OK && n < cap;
            if (!n) { tls_error(); ok = 0; }
        }
    } else {
        for (int done = 0, len = clen(req); done < len;) {
            int w = low_write(0, (u8 *)req + done, len - done);
            if (w < 0) { ok = 0; break; }
            done += w;
        }
        while (ok && n < cap) {
            int got = tcp_recv(sockfd, buf + n, (int)(cap - n > 16384 ? 16384 : cap - n));
            if (got > 0) { n += got; deadline = net_ms() + 20000; }
            else if (got == 0) { if (!wait_more()) { clean = 0; break; } }
            else { clean = got == -1; break; }
        }
        if (n >= cap) clean = 0;
        if (!n) ok = 0;
    }
    tcp_close(sockfd);
    if (!ok) return 0;

    u64 body = 0;
    for (u64 i = 0; i + 3 < n; i++) if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { body = i + 4; break; }
    if (!body || n < 12 || !prefix((char *)buf, "http/")) { net_set_error(u"HTTP の応答ではありません"); return 0; }
    r->status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
    const char *h = (char *)buf, *end = (char *)buf + body;
    char v[64];
    r->type[0] = 0; header(h, end, "content-type", r->type, sizeof r->type);
    location[0] = 0; header(h, end, "location", location, URLLEN);
    for (const char *l = h; l < end;) { /* Set-Cookie は何行もありうる */
        const char *e = l; while (e < end && *e != '\n') e++;
        if (e - l > 11 && prefix(l, "set-cookie:")) {
            static char line[1024]; int k = 0; const char *q = l + 11; while (q < e && *q == ' ') q++;
            while (q < e && *q != '\r' && k < 1023) line[k++] = *q++;
            line[k] = 0;
            cookie_from_response(host, *path ? path : "/", line);
        }
        l = e + 1;
    }
    int chunked = header(h, end, "transfer-encoding", v, sizeof v) && prefix(v, "chunked");
    i64 want = -1;
    if (header(h, end, "content-length", v, sizeof v)) { want = 0; for (char *p = v; *p >= '0' && *p <= '9' && want < (1LL << 40); p++) want = want * 10 + *p - '0'; }
    n -= body;
    mcopy(buf, buf + body, n);
    if (chunked) { u64 m; r->truncated = !dechunk(buf, n, &m); n = m; }
    else if (want >= 0) { r->truncated = n < (u64)want; if (n > (u64)want) n = want; }
    else r->truncated = !clean;
    r->len = n;
    return 1;
}
void url_join(const char *base, const char *ref, char *out); /* browser.c */
int http_get(const char *url, u8 *buf, u64 cap, HttpResult *r) {
    static char cur[URLLEN], location[URLLEN], next[URLLEN];
    cur[0] = 0; cadd(cur, url, URLLEN);
    net_set_error(u"通信に失敗しました");
    for (int hop = 0; hop < 8; hop++) {
        if (!once(cur, buf, cap, r, location)) return 0;
        if (r->status >= 300 && r->status < 400 && location[0]) {
            url_join(cur, location, next);
            cur[0] = 0; cadd(cur, next, URLLEN);
            continue;
        }
        r->url[0] = 0; cadd(r->url, cur, URLLEN);
        return 1;
    }
    net_set_error(u"転送が繰り返されています");
    return 0;
}
