/* js.c を Mac / Linux の上で直接試す: cc -Isrc -Isrc/libc tests/jstest.c src/js.c -o build/jstest && build/jstest file.js */
#include "js.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
SystemTable *ST;
static Status fake_time(Time *t, void *c) {
    (void)c; time_t now = time(0); struct tm *g = gmtime(&now);
    t->year = g->tm_year + 1900; t->month = g->tm_mon + 1; t->day = g->tm_mday; t->hour = g->tm_hour; t->minute = g->tm_min; t->second = g->tm_sec; t->ns = 0;
    return 0;
}
void mcopy(void *d, const void *s, u64 n) { u8 *p = d; const u8 *q = s; if (p <= q) while (n--) *p++ = *q++; else while (n) { n--; p[n] = q[n]; } }
JsVal host_get(void *node, const JsStr *key, int *found) { (void)node; (void)key; *found = 0; return js_undef(); }
int host_set(void *node, const JsStr *key, JsVal v) { (void)node; (void)key; (void)v; return 0; }
void *host_jsobj_slot(void *node, JsObj ***slot) { static JsObj *s; (void)node; *slot = &s; return 0; }
static void put16(const c16 *s, int n) {
    for (int i = 0; i < n; i++) { u32 c = s[i];
        if (c < 0x80) putchar(c); else if (c < 0x800) putchar(0xC0 | c >> 6), putchar(0x80 | (c & 63));
        else putchar(0xE0 | c >> 12), putchar(0x80 | ((c >> 6) & 63)), putchar(0x80 | (c & 63)); }
}
static JsVal log_(JsVal self, JsVal *a, int n) { (void)self; for (int i = 0; i < n; i++) { JsStr *s = js_tostr(a[i]); if (i) putchar(' '); put16(s->s, s->len); } putchar('\n'); return js_undef(); }
int main(int argc, char **argv) {
    static RuntimeServices rt; static SystemTable st; rt.GetTime = fake_time; st.rt = &rt; ST = &st;
    FILE *f = fopen(argv[1], "rb"); static unsigned char buf[1 << 20]; int n = (int)fread(buf, 1, sizeof buf, f); fclose(f);
    static c16 src[1 << 20]; int m = 0;
    for (int i = 0; i < n;) { u32 c = buf[i++]; if (c >= 0xE0) c = (c & 15) << 12 | (buf[i] & 63) << 6 | (buf[i + 1] & 63), i += 2; else if (c >= 0xC0) c = (c & 31) << 6 | (buf[i++] & 63); src[m++] = (c16)c; }
    void *mem = malloc(64 << 20);
    js_reset(mem, 64 << 20);
    JsVal con = js_object(); js_set(con, "log", js_native(log_)); js_global("console", con);
    extern u64 js_heap_used(void);
    int ok = js_run(src, m); printf("[heap %llu KB]\n", js_heap_used() / 1024); if (!ok) { printf("ERROR: "); const c16 *e = js_error(); int k = 0; while (e[k]) k++; put16(e, k); putchar('\n'); return 1; }
    return 0;
}
