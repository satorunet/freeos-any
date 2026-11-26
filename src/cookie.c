/* クッキー: サーバーの Set-Cookie を覚えて次から送る。document.cookie からも読み書きできる。
 * HOME\COOKIES.TXT に保存するので、再起動しても残る (期限切れは捨てる)。 */
#include "net.h"

#define MAXC 300
typedef struct {
    char domain[96], path[96], name[64], value[512];
    i64 expires;            /* UNIX 秒。0 なら閉じるまで */
    u8 secure, httpOnly, hostOnly, used;
} Cookie;
static Cookie jar[MAXC];
static int loaded;

static int clen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void ccopy(char *d, const char *s, int cap) { int n = 0; while (s[n] && n < cap - 1) { d[n] = s[n]; n++; } d[n] = 0; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ieq(const char *a, const char *b, int n) { for (int i = 0; i < n; i++) if (lower(a[i]) != lower(b[i])) return 0; return 1; }

static i64 days_from_civil(int y, int m, int d) {
    y -= m <= 2; i64 era = (y >= 0 ? y : y - 399) / 400; int yoe = y - (int)(era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static i64 now_s(void) {
    Time t; if (ST->rt->GetTime(&t, 0)) return 0;
    return ((days_from_civil(t.year, t.month, t.day) * 24 + t.hour) * 60 + t.minute) * 60 + t.second;
}
/* "Wed, 21 Oct 2026 07:28:00 GMT" (- 区切りも) */
static i64 parse_date(const char *s) {
    static const char *mon = "janfebmaraprmayjunjulaugsepoctnovdec";
    int day = -1, month = -1, year = -1, hh = 0, mm = 0, ss = 0;
    for (const char *p = s; *p;) {
        while (*p && !((*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'z'))) p++;
        if (*p >= '0' && *p <= '9') {
            int v = 0, len = 0; while (*p >= '0' && *p <= '9') v = v * 10 + *p++ - '0', len++;
            if (*p == ':') { hh = v; p++; mm = 0; while (*p >= '0' && *p <= '9') mm = mm * 10 + *p++ - '0'; if (*p == ':') { p++; ss = 0; while (*p >= '0' && *p <= '9') ss = ss * 10 + *p++ - '0'; } }
            else if (len >= 3) year = v < 100 ? v + 2000 : v;
            else if (day < 0) day = v;
            else year = v + (v < 70 ? 2000 : 1900);
        } else if (*p) {
            const char *w = p; while ((*p | 32) >= 'a' && (*p | 32) <= 'z') p++;
            if (p - w >= 3) for (int i = 0; i < 12; i++) if (ieq(w, mon + i * 3, 3)) month = i + 1;
        }
    }
    if (day < 0 || month < 0 || year < 0) return 0;
    return ((days_from_civil(year, month, day) * 24 + hh) * 60 + mm) * 60 + ss;
}
static void save(void);
static void load(void) {
    if (loaded) return;
    loaded = 1;
    static u8 buf[MAXC * 800];
    i64 n = fs_read(u"\\COOKIES.TXT", buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    i64 now = now_s(); int k = 0;
    for (char *line = (char *)buf; *line && k < MAXC;) {
        char *end = line; while (*end && *end != '\n') end++;
        char save_ = *end; *end = 0;
        char *f[8]; int nf = 0; f[nf++] = line;
        for (char *p = line; *p && nf < 8; p++) if (*p == '\t') { *p = 0; f[nf++] = p + 1; }
        if (nf == 8) {
            Cookie *c = &jar[k];
            ccopy(c->domain, f[0], 96); ccopy(c->path, f[1], 96);
            i64 e = 0; for (char *p = f[2]; *p >= '0' && *p <= '9'; p++) e = e * 10 + *p - '0';
            c->expires = e; c->secure = f[3][0] == '1'; c->httpOnly = f[4][0] == '1'; c->hostOnly = f[5][0] == '1';
            ccopy(c->name, f[6], 64); ccopy(c->value, f[7], 512);
            c->used = e > now; /* 保存されるのは期限付きだけ */
            if (c->used) k++;
        }
        *end = save_;
        line = *end ? end + 1 : end;
    }
}
static void save(void) {
    static u8 buf[MAXC * 800]; u64 n = 0; i64 now = now_s();
    for (int i = 0; i < MAXC; i++) {
        Cookie *c = &jar[i];
        if (!c->used || !c->expires || c->expires <= now) continue;
        char e[24]; int k = 23; e[k] = 0; i64 v = c->expires; do e[--k] = '0' + v % 10; while (v /= 10);
        const char *f[8] = {c->domain, c->path, e + k, c->secure ? "1" : "0", c->httpOnly ? "1" : "0", c->hostOnly ? "1" : "0", c->name, c->value};
        for (int j = 0; j < 8; j++) { for (const char *p = f[j]; *p && n < sizeof buf - 4; p++) buf[n++] = (u8)*p; buf[n++] = j < 7 ? '\t' : '\n'; }
    }
    fs_write(u"\\COOKIES.TXT", buf, n);
}

static int domain_match(const Cookie *c, const char *host) {
    if (c->hostOnly) { int a = clen(c->domain), b = clen(host); return a == b && ieq(c->domain, host, a); }
    int a = clen(c->domain), b = clen(host);
    if (b < a || !ieq(host + b - a, c->domain, a)) return 0;
    return b == a || host[b - a - 1] == '.';
}
static int path_match(const Cookie *c, const char *path) {
    int a = clen(c->path);
    for (int i = 0; i < a; i++) if (path[i] != c->path[i]) return 0;
    return c->path[a - 1] == '/' || path[a] == 0 || path[a] == '/' || path[a] == '?';
}
/* 1 つ設定する (Set-Cookie の 1 行、または document.cookie への代入) */
static void set_one(const char *host, const char *path, const char *s, int fromScript) {
    load();
    char name[64], value[512], domain[96] = "", cpath[96] = ""; int secure = 0, httpOnly = 0; i64 expires = 0, now = now_s(); int hasMax = 0;
    const char *p = s; int k = 0;
    while (*p == ' ') p++;
    while (*p && *p != '=' && *p != ';' && k < 63) name[k++] = *p++;
    while (k && name[k - 1] == ' ') k--;
    name[k] = 0;
    if (*p != '=') return;
    p++; k = 0;
    while (*p && *p != ';' && k < 511) value[k++] = *p++;
    value[k] = 0;
    while (*p) { /* 属性 */
        while (*p == ';' || *p == ' ') p++;
        const char *a = p; while (*p && *p != '=' && *p != ';') p++;
        int al = (int)(p - a); while (al && a[al - 1] == ' ') al--;
        char v[128] = ""; int vl = 0;
        if (*p == '=') { p++; while (*p == ' ') p++; while (*p && *p != ';' && vl < 127) v[vl++] = *p++; v[vl] = 0; }
        if (al == 7 && ieq(a, "expires", 7) && !hasMax) expires = parse_date(v);
        else if (al == 7 && ieq(a, "max-age", 7)) { i64 m = 0; int neg = v[0] == '-'; for (char *q = v + neg; *q >= '0' && *q <= '9'; q++) m = m * 10 + *q - '0'; expires = neg || !m ? 1 : now + m; hasMax = 1; }
        else if (al == 6 && ieq(a, "domain", 6)) ccopy(domain, v[0] == '.' ? v + 1 : v, 96);
        else if (al == 4 && ieq(a, "path", 4)) ccopy(cpath, v, 96);
        else if (al == 6 && ieq(a, "secure", 6)) secure = 1;
        else if (al == 8 && ieq(a, "httponly", 8)) httpOnly = 1;
    }
    if (fromScript && httpOnly) return;
    int hostOnly = !domain[0];
    if (hostOnly) ccopy(domain, host, 96);
    else { Cookie t; ccopy(t.domain, domain, 96); t.hostOnly = 0; if (!domain_match(&t, host)) return; } /* 別のサイトのものは受け取らない */
    if (!cpath[0] || cpath[0] != '/') { /* 既定: 今のパスのフォルダ */
        int l = 0, last = 0; while (path[l] && path[l] != '?' && l < 95) { if (path[l] == '/') last = l; l++; }
        if (last == 0) ccopy(cpath, "/", 96); else { for (int i = 0; i < last; i++) cpath[i] = path[i]; cpath[last] = 0; }
    }
    int slot = -1, freeSlot = -1;
    for (int i = 0; i < MAXC; i++) {
        if (!jar[i].used) { if (freeSlot < 0) freeSlot = i; continue; }
        int a = clen(jar[i].domain);
        if (ieq(jar[i].domain, domain, a + 1) && !clen(jar[i].name) == !clen(name) && ieq(jar[i].name, name, clen(name) + 1) && ieq(jar[i].path, cpath, clen(cpath) + 1)) slot = i;
    }
    int expired = expires && expires <= now;
    if (slot >= 0 && expired) { jar[slot].used = 0; save(); return; }
    if (expired) return;
    if (slot < 0) slot = freeSlot >= 0 ? freeSlot : 0; /* いっぱいなら最初のものを捨てる */
    Cookie *c = &jar[slot];
    ccopy(c->domain, domain, 96); ccopy(c->path, cpath, 96); ccopy(c->name, name, 64); ccopy(c->value, value, 512);
    c->expires = expires; c->secure = (u8)secure; c->httpOnly = (u8)httpOnly; c->hostOnly = (u8)hostOnly; c->used = 1;
    if (expires) save();
}
void cookie_from_response(const char *host, const char *path, const char *line) { set_one(host, path, line, 0); }
/* 送るクッキー "a=1; b=2" */
int cookie_header(const char *host, const char *path, int https, char *out, int cap, int forScript) {
    load();
    int n = 0; i64 now = now_s(); out[0] = 0;
    for (int i = 0; i < MAXC; i++) {
        Cookie *c = &jar[i];
        if (!c->used) continue;
        if (c->expires && c->expires <= now) { c->used = 0; continue; }
        if (!domain_match(c, host) || !path_match(c, path) || (c->secure && !https) || (forScript && c->httpOnly)) continue;
        int need = clen(c->name) + clen(c->value) + 3;
        if (n + need >= cap) break;
        if (n) out[n++] = ';', out[n++] = ' ';
        for (const char *p = c->name; *p; p++) out[n++] = *p;
        if (c->name[0]) out[n++] = '=';
        for (const char *p = c->value; *p; p++) out[n++] = *p;
        out[n] = 0;
    }
    return n;
}
/* URL から host と path を取り出す */
static int split(const char *url, char *host, const char **path, int *https) {
    *https = url[4] == 's' || url[4] == 'S';
    const char *p = url; while (*p && !(p[0] == '/' && p[1] == '/')) p++;
    if (!*p) return 0;
    p += 2; int k = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' && k < 95) host[k++] = lower(*p++);
    host[k] = 0;
    while (*p && *p != '/' && *p != '?') p++;
    *path = *p == '/' ? p : "/";
    return k > 0;
}
int cookie_doc_get(const char *url, char *out, int cap) {
    char host[96]; const char *path; int https;
    out[0] = 0;
    if (!split(url, host, &path, &https)) return 0;
    return cookie_header(host, path, https, out, cap, 1);
}
void cookie_doc_set(const char *url, const char *s) {
    char host[96]; const char *path; int https;
    if (split(url, host, &path, &https)) set_one(host, path, s, 1);
}
