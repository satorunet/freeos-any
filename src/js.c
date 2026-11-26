/* 小さな JavaScript 実行エンジン。字句解析 → 構文木 → 木をたどって実行する。
 * 対応: var/let/const、関数 (アロー関数も)、if/for/for-of/for-in/while/do/switch/try/throw、
 * 数・文字列・真偽・null・undefined・配列・オブジェクト・テンプレート文字列、主な組み込み関数。
 * 非対応: class、正規表現 (文字列として扱う)、ジェネレーター、async、getter/setter など。 */
#include "js.h"

/* ---- メモリ (ページごとにまとめて捨てる) ---- */
static u8 *heap; static u64 heapSize, heapUsed;
static int oom;
static void *alloc(u64 n) {
    n = (n + 7) & ~7ull;
    if (heapUsed + n > heapSize) { oom = 1; heapUsed = 0; } /* 使い切ったら最初から (壊れるが止まらない) */
    void *p = heap + heapUsed; heapUsed += n;
    memset(p, 0, n);
    return p;
}

/* ---- 値 ---- */
typedef struct Prop { JsStr *k; JsVal v; struct Prop *next; } Prop;
typedef struct Node Node;
typedef struct Env { Prop *vars; struct Env *up; JsVal self; int hasSelf; } Env;
enum { K_OBJ, K_ARR, K_FUNC, K_NATIVE, K_HOST, K_DATE, K_ERR };
struct JsObj {
    u8 kind;
    Prop *props;
    JsVal *arr; int len, cap;   /* 配列 */
    Node *fn; Env *env;         /* 関数 */
    JsNative nat; JsVal bound;  /* 組み込み関数 (bound は this) */
    void *host;                 /* 要素 */
    double ms;                  /* 日付 */
    JsObj *proto;
};
static JsVal V(u8 t) { JsVal v = {t, 0, 0}; return v; }
JsVal js_undef(void) { return V(JS_UNDEF); }
static JsVal jnull(void) { return V(JS_NULL); }
JsVal js_num(double n) { JsVal v = {JS_NUM, n, 0}; return v; }
JsVal js_bool(int b) { JsVal v = {JS_BOOL, b ? 1 : 0, 0}; return v; }
static JsStr *newstr(int len) { JsStr *s = alloc(sizeof(JsStr) + (u64)(len + 1) * 2); s->len = len; return s; }
static JsStr *mkstr(const c16 *p, int len) { JsStr *s = newstr(len); for (int i = 0; i < len; i++) s->s[i] = p[i]; return s; }
static JsStr *cstr(const char *p) { /* UTF-8 から */
    const u8 *q = (const u8 *)p; int n = 0;
    for (int i = 0; q[i]; i++) if ((q[i] & 0xC0) != 0x80) n++;
    JsStr *s = newstr(n); int k = 0;
    while (*q) {
        u32 c = *q++;
        if (c >= 0xE0) { c = (c & 15) << 12 | (q[0] & 63) << 6 | (q[1] & 63); q += 2; }
        else if (c >= 0xC0) { c = (c & 31) << 6 | (q[0] & 63); q++; }
        s->s[k++] = (c16)c;
    }
    return s;
}
static JsVal S(JsStr *s) { JsVal v = {JS_STR, 0, s}; return v; }
JsVal js_str(const c16 *p, int len) { return S(mkstr(p, len)); }
JsVal js_cstr(const char *p) { return S(cstr(p)); }
static JsObj *newobj(u8 kind) { JsObj *o = alloc(sizeof(JsObj)); o->kind = kind; return o; }
static JsVal O(JsObj *o) { JsVal v = {JS_OBJ, 0, o}; return v; }
JsVal js_object(void) { return O(newobj(K_OBJ)); }
JsVal js_native(JsNative f) { JsObj *o = newobj(K_NATIVE); o->nat = f; return O(o); }
static JsVal bound_native(JsNative f, JsVal self) { JsObj *o = newobj(K_NATIVE); o->nat = f; o->bound = self; return O(o); }
static JsObj *obj(JsVal v) { return v.t == JS_OBJ ? (JsObj *)v.p : 0; }
int js_is_func(JsVal v) { JsObj *o = obj(v); return o && (o->kind == K_FUNC || o->kind == K_NATIVE); }
void *js_host_node(JsVal v) { JsObj *o = obj(v); return o && o->kind == K_HOST ? o->host : 0; }
JsVal js_host(void *node) {
    JsObj **slot; host_jsobj_slot(node, &slot);
    if (!*slot) { *slot = newobj(K_HOST); (*slot)->host = node; }
    return O(*slot);
}
static int streq(const JsStr *a, const JsStr *b) {
    if (a->len != b->len) return 0;
    for (int i = 0; i < a->len; i++) if (a->s[i] != b->s[i]) return 0;
    return 1;
}
int js_key_is(const JsStr *k, const char *s) {
    int i = 0;
    for (; s[i]; i++) if (i >= k->len || k->s[i] != (u8)s[i]) return 0;
    return i == k->len;
}
static JsStr *concat(JsStr *a, JsStr *b) {
    JsStr *s = newstr(a->len + b->len);
    for (int i = 0; i < a->len; i++) s->s[i] = a->s[i];
    for (int i = 0; i < b->len; i++) s->s[a->len + i] = b->s[i];
    return s;
}

/* ---- 数学 ---- */
static double dfloor(double x) { if (x != x || x > 9e15 || x < -9e15) return x; i64 i = (i64)x; return (double)i > x ? (double)(i - 1) : (double)i; }
static double dsqrt(double x) {
    if (x <= 0) return x == 0 ? 0 : 0.0 / 0.0;
#if defined(__aarch64__)
    double r; __asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x)); return r;
#else
    double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r;
#endif
}
static double dexp(double x) {
    if (x > 700) return 1.0 / 0.0;
    if (x < -700) return 0;
    int n = 0; while (x > 0.5) x -= 0.5, n++; while (x < -0.5) x += 0.5, n--;
    double t = 1, r = 1; for (int k = 1; k < 20; k++) { t *= x / k; r += t; }
    for (; n > 0; n--) r *= 1.6487212707001282; for (; n < 0; n++) r /= 1.6487212707001282;
    return r;
}
static double dlog(double x) {
    if (x <= 0) return x == 0 ? -1.0 / 0.0 : 0.0 / 0.0;
    int e = 0; while (x > 2) x /= 2, e++; while (x < 1) x *= 2, e--;
    double y = (x - 1) / (x + 1), y2 = y * y, t = y, r = 0;
    for (int k = 1; k < 40; k += 2) { r += t / k; t *= y2; }
    return 2 * r + e * 0.6931471805599453;
}
static double dpow(double a, double b) {
    if (b == dfloor(b) && b > -1000 && b < 1000) {
        double r = 1, x = a; i64 n = (i64)(b < 0 ? -b : b);
        while (n) { if (n & 1) r *= x; x *= x; n >>= 1; }
        return b < 0 ? 1 / r : r;
    }
    return dexp(b * dlog(a));
}
static double dsin(double x) {
    const double PI = 3.14159265358979323846;
    x = x - 2 * PI * dfloor(x / (2 * PI) + 0.5);
    double x2 = x * x, t = x, r = x;
    for (int k = 1; k < 14; k++) { t *= -x2 / ((2 * k) * (2 * k + 1)); r += t; }
    return r;
}
static u64 rngState = 88172645463325252ull;
static double drand(void) { rngState ^= rngState << 13; rngState ^= rngState >> 7; rngState ^= rngState << 17; return (double)(rngState >> 11) / 9007199254740992.0; }

/* ---- 文字列と数の変換 ---- */
static JsStr *numstr(double v) {
    c16 b[40]; int n = 0;
    if (v != v) return cstr("NaN");
    if (v == 1.0 / 0.0) return cstr("Infinity");
    if (v == -1.0 / 0.0) return cstr("-Infinity");
    if (v == 0) return cstr("0");
    if (v < 0) b[n++] = '-', v = -v;
    if (v < 1e15 && v == dfloor(v)) {
        c16 t[24]; int k = 0; u64 u = (u64)v;
        do t[k++] = '0' + u % 10, u /= 10; while (u);
        while (k) b[n++] = t[--k];
        return mkstr(b, n);
    }
    int e = 0; double m = v;
    while (m >= 10) m /= 10, e++;
    while (m < 1) m *= 10, e--;
    int dig[18], P = 15;
    for (P = 15; P <= 17; P++) { /* 戻したとき同じ数になる一番短い桁数 */
        double scale = dpow(10, e - P + 1);
        u64 d = (u64)(v / scale + 0.5), lim = 1; for (int i = 0; i < P; i++) lim *= 10;
        int ee = e; if (d >= lim) { d /= 10; ee++; }
        for (int i = P - 1; i >= 0; i--) dig[i] = d % 10, d /= 10;
        double back = 0; for (int i = 0; i < P; i++) back = back * 10 + dig[i];
        back *= dpow(10, ee - P + 1);
        if (back == v || P == 17) { e = ee; break; }
    }
    for (int i = P; i < 18; i++) dig[i] = 0;
    int last = P - 1; while (last > 0 && dig[last] == 0) last--;
    if (e >= -6 && e < 21) {
        if (e < 0) { b[n++] = '0'; b[n++] = '.'; for (int i = -1; i > e; i--) b[n++] = '0'; for (int i = 0; i <= last; i++) b[n++] = '0' + dig[i]; }
        else { for (int i = 0; i <= e || i <= last; i++) { if (i == e + 1) b[n++] = '.'; b[n++] = '0' + (i < 18 ? dig[i] : 0); if (i >= e && i >= last) break; } }
    } else {
        b[n++] = '0' + dig[0];
        if (last) { b[n++] = '.'; for (int i = 1; i <= last; i++) b[n++] = '0' + dig[i]; }
        b[n++] = 'e'; b[n++] = e < 0 ? '-' : '+'; if (e < 0) e = -e;
        c16 t[6]; int k = 0; do t[k++] = '0' + e % 10, e /= 10; while (e); while (k) b[n++] = t[--k];
    }
    return mkstr(b, n);
}
static int isspace16(c16 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || c == 0x3000 || c == 0xFEFF; }
static double strnum(const JsStr *s) {
    int i = 0, n = s->len;
    while (i < n && isspace16(s->s[i])) i++;
    while (n > i && isspace16(s->s[n - 1])) n--;
    if (i == n) return 0;
    int neg = 0; if (s->s[i] == '-' || s->s[i] == '+') neg = s->s[i++] == '-';
    if (n - i == 8 && s->s[i] == 'I') return neg ? -1.0 / 0.0 : 1.0 / 0.0;
    double v = 0; int any = 0;
    if (n - i > 2 && s->s[i] == '0' && (s->s[i + 1] | 32) == 'x') {
        for (i += 2; i < n; i++) { c16 c = s->s[i] | 32; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; if (d < 0) return 0.0 / 0.0; v = v * 16 + d; any = 1; }
        return any ? v : 0.0 / 0.0;
    }
    for (; i < n && s->s[i] >= '0' && s->s[i] <= '9'; i++) v = v * 10 + (s->s[i] - '0'), any = 1;
    if (i < n && s->s[i] == '.') { double f = 0.1; for (i++; i < n && s->s[i] >= '0' && s->s[i] <= '9'; i++) v += (s->s[i] - '0') * f, f /= 10, any = 1; }
    if (any && i < n && (s->s[i] | 32) == 'e') {
        i++; int en = 0, es = 1; if (i < n && (s->s[i] == '-' || s->s[i] == '+')) es = s->s[i++] == '-' ? -1 : 1;
        for (; i < n && s->s[i] >= '0' && s->s[i] <= '9'; i++) en = en * 10 + s->s[i] - '0';
        v *= dpow(10, es * en);
    }
    if (!any || i != n) return 0.0 / 0.0;
    return neg ? -v : v;
}
static JsVal call(JsVal f, JsVal self, JsVal *args, int n);
static JsVal getp(JsVal o, JsStr *k);
double js_tonum(JsVal v) {
    switch (v.t) {
    case JS_NUM: return v.n;
    case JS_BOOL: return v.n;
    case JS_NULL: return 0;
    case JS_STR: return strnum(v.p);
    case JS_OBJ: { JsObj *o = v.p; if (o->kind == K_DATE) return o->ms; if (o->kind == K_ARR) { if (!o->len) return 0; if (o->len == 1) return js_tonum(o->arr[0]); } return 0.0 / 0.0; }
    default: return 0.0 / 0.0;
    }
}
static JsStr *join(JsObj *a, JsStr *sep);
static JsStr *date_str(double ms, int what);
JsStr *js_tostr(JsVal v) {
    switch (v.t) {
    case JS_UNDEF: return cstr("undefined");
    case JS_NULL: return cstr("null");
    case JS_BOOL: return cstr(v.n ? "true" : "false");
    case JS_NUM: return numstr(v.n);
    case JS_STR: return v.p;
    default: {
        JsObj *o = v.p;
        if (o->kind == K_ARR) return join(o, cstr(","));
        if (o->kind == K_FUNC || o->kind == K_NATIVE) return cstr("function () { [code] }");
        if (o->kind == K_HOST) return cstr("[object HTMLElement]");
        if (o->kind == K_DATE) return date_str(o->ms, 0);
        if (o->kind == K_ERR) { JsVal m = getp(v, cstr("message")); return concat(cstr("Error: "), js_tostr(m)); }
        JsVal ts = getp(v, cstr("toString"));
        if (js_is_func(ts) && obj(ts)->kind == K_FUNC) { JsVal r = call(ts, v, 0, 0); if (r.t == JS_STR) return r.p; }
        return cstr("[object Object]");
    }
    }
}
int js_truthy(JsVal v) {
    switch (v.t) {
    case JS_UNDEF: case JS_NULL: return 0;
    case JS_BOOL: return v.n != 0;
    case JS_NUM: return v.n == v.n && v.n != 0;
    case JS_STR: return ((JsStr *)v.p)->len > 0;
    default: return 1;
    }
}

/* ---- 字句解析 ---- */
enum { T_EOF, T_NUM, T_STR, T_ID, T_P };
typedef struct { u8 t, nl; char p[5]; int line; double n; JsStr *s; } Tok;
static Tok *toks; static int ntok, tp, parsing;
static int curLine(void) { return parsing && toks && tp < ntok ? toks[tp].line : 0; }
static c16 errmsg[200];
static int failed;
static int curLine(void);
static void fail(const char *m) {
    if (failed) return;
    failed = 1; int i = 0;
    for (const u8 *p = (const u8 *)m; *p && i < 170;) { /* UTF-8 → UTF-16 */
        u32 c = *p++;
        if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (c >= 0xC0) { c = (c & 31) << 6 | (p[0] & 63); p++; }
        errmsg[i++] = (c16)c;
    }
    int line = curLine();
    if (line) {
        const char *t = " (行 "; for (const u8 *p = (const u8 *)t; *p;) { u32 c = *p++; if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; } errmsg[i++] = (c16)c; }
        c16 d[8]; int k = 0; do d[k++] = '0' + line % 10, line /= 10; while (line); while (k) errmsg[i++] = d[--k];
        errmsg[i++] = ')';
    }
    errmsg[i] = 0;
}
static int pis(const Tok *t, const char *p) { if (t->t != T_P) return 0; int i = 0; for (; p[i]; i++) if (t->p[i] != p[i]) return 0; return !t->p[i]; }
static int idis(const Tok *t, const char *s) { return t->t == T_ID && js_key_is(t->s, s); }
static int isid(c16 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' || c > 127; }
static const char *const puncts[] = {">>>=", "===", "!==", "**=", "...", "<<=", ">>=", ">>>", "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.",
    "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "**", "<<", ">>", 0};
static int lexLine;
static Tok *push_tok(u8 t, int nl) {
    if (ntok % 256 == 0) { Tok *n = alloc(sizeof(Tok) * (u64)(ntok + 256)); if (ntok) mcopy(n, toks, sizeof(Tok) * (u64)ntok); toks = n; }
    Tok *k = &toks[ntok++]; k->t = t; k->nl = (u8)nl; k->line = lexLine; return k;
}
static int regex_ok(void) { /* ここで / が来たら正規表現か */
    if (!ntok) return 1;
    Tok *p = &toks[ntok - 1];
    if (p->t == T_NUM || p->t == T_STR) return 0;
    if (p->t == T_ID) return idis(p, "return") || idis(p, "typeof") || idis(p, "case") || idis(p, "in") || idis(p, "of");
    return !pis(p, ")") && !pis(p, "]") && !pis(p, "}");
}
static int lex(const c16 *s, int n) {
    toks = 0; ntok = 0; lexLine = 1;
    int i = 0, nl = 0;
    int tdepth[16], ntd = 0; /* テンプレート文字列の ${ の深さ */
    while (i <= n && !failed) {
        if (i == n) { push_tok(T_EOF, 1); break; }
        c16 c = s[i];
        if (c == '\n') { nl = 1; i++; lexLine++; continue; }
        if (isspace16(c)) { i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { while (i < n && s[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { i += 2; while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) { if (s[i] == '\n') nl = 1, lexLine++; i++; } i += 2; continue; }
        if (c == '<' && i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') { while (i < n && s[i] != '\n') i++; continue; } /* <!-- */
        if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9')) {
            int st = i;
            if (c == '0' && i + 1 < n && (s[i + 1] | 32) == 'x') { i += 2; while (i < n && isid(s[i])) i++; }
            else { int dot = 0; while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '_' || (s[i] == '.' && !dot++))) i++;
                   if (i < n && (s[i] | 32) == 'e') { i++; if (s[i] == '+' || s[i] == '-') i++; while (i < n && s[i] >= '0' && s[i] <= '9') i++; } }
            c16 b[64]; int k = 0; for (int j = st; j < i && k < 63; j++) if (s[j] != '_') b[k++] = s[j];
            Tok *t = push_tok(T_NUM, nl); JsStr tmp; (void)tmp;
            t->n = strnum(mkstr(b, k)); nl = 0; continue;
        }
        if (isid(c)) { int st = i; while (i < n && isid(s[i])) i++; Tok *t = push_tok(T_ID, nl); t->s = mkstr(s + st, i - st); nl = 0; continue; }
        if (c == '"' || c == '\'' || c == '`' || (c == '}' && ntd && tdepth[ntd - 1] == 0)) {
            int tmpl = c == '`' || c == '}';
            if (c == '}') ntd--; /* ${ ... } の終わり: 続きの文字列へ */
            else if (tmpl) push_tok(T_P, nl)->p[0] = '(';
            if (c == '}') { Tok *t = push_tok(T_P, 0); t->p[0] = ')'; push_tok(T_P, 0)->p[0] = '+'; }
            c16 q = tmpl ? '`' : c; i++;
            c16 *b = alloc((u64)(n - i + 1) * 2); int k = 0, open = 0;
            while (i < n && s[i] != q) {
                if (tmpl && s[i] == '$' && i + 1 < n && s[i + 1] == '{') { open = 1; i += 2; break; }
                c16 ch = s[i++];
                if (ch == '\\' && i < n) {
                    ch = s[i++];
                    if (ch == 'n') ch = '\n'; else if (ch == 't') ch = '\t'; else if (ch == 'r') ch = '\r'; else if (ch == 'b') ch = 8; else if (ch == '0') ch = 0;
                    else if (ch == 'u' || ch == 'x') {
                        int len = ch == 'u' ? 4 : 2; u32 v = 0;
                        if (ch == 'u' && i < n && s[i] == '{') { i++; while (i < n && s[i] != '}') { c16 h = s[i++] | 32; v = v * 16 + (h <= '9' ? h - '0' : h - 'a' + 10); } i++; }
                        else for (int j = 0; j < len && i < n; j++) { c16 h = s[i++] | 32; v = v * 16 + (h <= '9' ? h - '0' : h - 'a' + 10); }
                        ch = v > 0xFFFF ? 0xFFFD : (c16)v;
                    } else if (ch == '\n') continue;
                }
                b[k++] = ch;
            }
            Tok *t = push_tok(T_STR, tmpl ? 0 : nl); t->s = mkstr(b, k);
            if (open) { push_tok(T_P, 0)->p[0] = '+'; push_tok(T_P, 0)->p[0] = '('; if (ntd < 16) tdepth[ntd++] = 0; }
            else { i++; if (tmpl) push_tok(T_P, 0)->p[0] = ')'; }
            nl = 0; continue;
        }
        if (c == '/' && regex_ok()) { /* 正規表現は中身の文字列として扱う */
            int st = ++i, cls = 0;
            while (i < n && (s[i] != '/' || cls) && s[i] != '\n') { if (s[i] == '\\') i++; else if (s[i] == '[') cls = 1; else if (s[i] == ']') cls = 0; i++; }
            Tok *t = push_tok(T_STR, nl); t->s = mkstr(s + st, i - st); i++;
            while (i < n && isid(s[i])) i++;
            nl = 0; continue;
        }
        Tok *t = push_tok(T_P, nl); nl = 0;
        int done = 0;
        for (int p = 0; puncts[p] && !done; p++) {
            int L = 0; while (puncts[p][L] && i + L < n && s[i + L] == (u8)puncts[p][L]) L++;
            if (!puncts[p][L]) { for (int j = 0; j < L; j++) t->p[j] = puncts[p][j]; i += L; done = 1; }
        }
        if (done) {
            if (pis(t, "?.") && i < n && s[i] >= '0' && s[i] <= '9') { t->p[1] = 0; i--; } /* a?.5:b */
            continue;
        }
        if (c == '{' && ntd) tdepth[ntd - 1]++;
        if (c == '}' && ntd) tdepth[ntd - 1]--;
        t->p[0] = (char)(c < 128 ? c : '?'); i++;
    }
    return !failed;
}

/* ---- 構文木 ---- */
enum { N_NUM, N_STR, N_ID, N_THIS, N_ARR, N_OBJ, N_PROP, N_FUNC, N_CALL, N_NEW, N_MEMBER, N_INDEX, N_UNARY, N_BIN, N_LOGIC,
       N_ASSIGN, N_UPDATE, N_COND, N_SEQ, N_SPREAD,
       N_VAR, N_DECL, N_EXPR, N_IF, N_FOR, N_FORIN, N_WHILE, N_DO, N_BLOCK, N_RETURN, N_BREAK, N_CONT, N_THROW, N_TRY, N_SWITCH, N_CASE, N_EMPTY };
struct Node { u8 t, flag; char op[5]; Node *a, *b, *c, *d, *next; JsStr *s; double n; };
static Node *node(u8 t) { Node *n = alloc(sizeof(Node)); n->t = t; return n; }
static Tok *cur(void) { return &toks[tp]; }
static Tok *peek(int k) { return &toks[tp + k < ntok ? tp + k : ntok - 1]; }
static int accept(const char *p) { if (pis(cur(), p)) { tp++; return 1; } return 0; }
static void expect(const char *p) { if (!accept(p)) { static char m[40] = "構文エラー: "; int i = 0; while (p[i] && i < 4) m[16 + i] = p[i], i++; m[16 + i] = 0; fail("構文エラー"); } }
static int accept_kw(const char *k) { if (idis(cur(), k)) { tp++; return 1; } return 0; }
static JsStr *ident(void) {
    if (cur()->t != T_ID) { fail("構文エラー: 名前がありません"); return cstr("?"); }
    return toks[tp++].s;
}
static void semi(void) {
    if (accept(";")) return;
    if (pis(cur(), "}") || cur()->t == T_EOF || cur()->nl) return;
    fail("構文エラー: ; がありません");
}
static Node *expr(void);
static Node *assign(void);
static Node *statement(void);
static Node *block_body(void) { /* { の次から } まで */
    Node *head = 0, **tail = &head;
    while (!failed && !pis(cur(), "}") && cur()->t != T_EOF) { *tail = statement(); if (*tail) tail = &(*tail)->next; }
    expect("}");
    return head;
}
static Node *function_rest(JsStr *name, int arrow) { /* ( から */
    Node *f = node(N_FUNC); f->s = name;
    Node **tail = &f->a;
    expect("(");
    while (!failed && !pis(cur(), ")")) {
        Node *p = node(N_ID);
        if (accept("...")) p->flag = 1;
        p->s = ident();
        if (accept("=")) p->b = assign();
        *tail = p; tail = &p->next;
        if (!accept(",")) break;
    }
    expect(")");
    if (arrow) expect("=>");
    if (arrow && !pis(cur(), "{")) { f->flag = 3; f->b = assign(); return f; } /* 式だけの本体 */
    expect("{");
    f->b = node(N_BLOCK); f->b->a = block_body();
    f->flag = arrow ? 1 : 0;
    return f;
}
static int arrow_ahead(void) { /* ( ... ) => か */
    if (!pis(cur(), "(")) return 0;
    int d = 0;
    for (int k = tp; k < ntok; k++) {
        if (pis(&toks[k], "(")) d++;
        else if (pis(&toks[k], ")") && --d == 0) return pis(&toks[k + 1], "=>");
        else if (toks[k].t == T_EOF) return 0;
    }
    return 0;
}
static Node *primary(void) {
    Tok *t = cur();
    if (t->t == T_NUM) { tp++; Node *n = node(N_NUM); n->n = t->n; return n; }
    if (t->t == T_STR) { tp++; Node *n = node(N_STR); n->s = t->s; return n; }
    if (t->t == T_ID) {
        if (pis(peek(1), "=>")) { /* x => ... */
            Node *f = node(N_FUNC); f->a = node(N_ID); f->a->s = t->s; tp += 2;
            if (pis(cur(), "{")) { tp++; f->b = node(N_BLOCK); f->b->a = block_body(); f->flag = 1; }
            else { f->b = assign(); f->flag = 3; }
            return f;
        }
        if (idis(t, "function")) { tp++; JsStr *name = cur()->t == T_ID ? ident() : 0; return function_rest(name, 0); }
        if (idis(t, "async") && (idis(peek(1), "function") || pis(peek(1), "("))) { tp++; return primary(); }
        if (idis(t, "this")) { tp++; return node(N_THIS); }
        if (idis(t, "new")) {
            tp++; Node *n = node(N_NEW);
            Node *callee = primary();
            while (!failed) { if (accept(".")) { Node *m = node(N_MEMBER); m->a = callee; m->s = ident(); callee = m; } else break; }
            n->a = callee;
            if (accept("(")) { Node **tail = &n->b; while (!failed && !pis(cur(), ")")) { *tail = assign(); tail = &(*tail)->next; if (!accept(",")) break; } expect(")"); }
            return n;
        }
        tp++; Node *n = node(N_ID); n->s = t->s; return n;
    }
    if (arrow_ahead()) return function_rest(0, 1);
    if (accept("(")) { Node *e = expr(); expect(")"); return e; }
    if (accept("[")) {
        Node *n = node(N_ARR), **tail = &n->a;
        while (!failed && !pis(cur(), "]")) {
            Node *e;
            if (pis(cur(), ",")) e = node(N_ID), e->s = cstr("undefined");
            else if (accept("...")) { e = node(N_SPREAD); e->a = assign(); }
            else e = assign();
            *tail = e; tail = &e->next;
            if (!accept(",")) break;
        }
        expect("]");
        return n;
    }
    if (accept("{")) {
        Node *n = node(N_OBJ), **tail = &n->a;
        while (!failed && !pis(cur(), "}")) {
            Node *p = node(N_PROP);
            if (accept("...")) { p->flag = 2; p->b = assign(); }
            else {
                if (accept("[")) { p->c = assign(); expect("]"); }
                else if (cur()->t == T_STR || cur()->t == T_ID) p->s = toks[tp++].s;
                else if (cur()->t == T_NUM) p->s = numstr(toks[tp++].n);
                else { fail("構文エラー: オブジェクト"); break; }
                if (pis(cur(), "(")) p->b = function_rest(p->s, 0);       /* メソッド */
                else if (accept(":")) p->b = assign();
                else { p->b = node(N_ID); p->b->s = p->s; }           /* 省略形 {a} */
            }
            *tail = p; tail = &p->next;
            if (!accept(",")) break;
        }
        expect("}");
        return n;
    }
    fail("構文エラー: 予期しない記号");
    tp++;
    return node(N_EMPTY);
}
static Node *postfix(void) {
    Node *e = primary();
    for (;;) {
        if (failed) return e;
        if (accept(".") || accept("?.")) {
            if (pis(cur(), "(")) { continue; } /* a?.() */
            Node *m = node(N_MEMBER); m->a = e; m->s = ident(); e = m;
        } else if (accept("[")) { Node *m = node(N_INDEX); m->a = e; m->b = expr(); expect("]"); e = m; }
        else if (accept("(")) {
            Node *c = node(N_CALL); c->a = e; Node **tail = &c->b;
            while (!failed && !pis(cur(), ")")) {
                Node *x; if (accept("...")) { x = node(N_SPREAD); x->a = assign(); } else x = assign();
                *tail = x; tail = &x->next; if (!accept(",")) break;
            }
            expect(")"); e = c;
        } else if (cur()->t == T_STR && toks[tp - 1].t != T_P) { break; }
        else if ((pis(cur(), "++") || pis(cur(), "--")) && !cur()->nl) {
            Node *u = node(N_UPDATE); u->op[0] = toks[tp++].p[0]; u->flag = 0; u->a = e; e = u;
        } else break;
    }
    return e;
}
static Node *unary(void) {
    Tok *t = cur();
    if (pis(t, "!") || pis(t, "-") || pis(t, "+") || pis(t, "~")) { tp++; Node *u = node(N_UNARY); u->op[0] = t->p[0]; u->a = unary(); return u; }
    if (pis(t, "++") || pis(t, "--")) { tp++; Node *u = node(N_UPDATE); u->op[0] = t->p[0]; u->flag = 1; u->a = unary(); return u; }
    if (idis(t, "typeof") || idis(t, "void") || idis(t, "delete") || idis(t, "await")) {
        tp++; Node *u = node(N_UNARY); u->op[0] = t->s->s[0] == 't' ? 't' : t->s->s[0] == 'v' ? 'v' : t->s->s[0] == 'd' ? 'd' : 'a'; u->a = unary(); return u;
    }
    return postfix();
}
static int prec(Tok *t, int *right) {
    *right = 0;
    if (t->t == T_ID) return idis(t, "instanceof") || idis(t, "in") ? 8 : 0;
    if (t->t != T_P) return 0;
    static const struct { const char *p; int pr; } ops[] = {
        {"??", 1}, {"||", 2}, {"&&", 3}, {"|", 4}, {"^", 5}, {"&", 6}, {"==", 7}, {"!=", 7}, {"===", 7}, {"!==", 7},
        {"<", 8}, {">", 8}, {"<=", 8}, {">=", 8}, {"<<", 9}, {">>", 9}, {">>>", 9}, {"+", 10}, {"-", 10}, {"*", 11}, {"/", 11}, {"%", 11}, {"**", 12}};
    for (unsigned i = 0; i < sizeof ops / sizeof *ops; i++) if (pis(t, ops[i].p)) { *right = ops[i].pr == 12; return ops[i].pr; }
    return 0;
}
static int noIn; /* for (x in ...) の中では in を演算子にしない */
static Node *binary(int min) {
    Node *l = unary();
    for (;;) {
        int right, p = prec(cur(), &right);
        if (!p || p < min || failed) break;
        if (noIn && idis(cur(), "in")) break;
        Tok *t = &toks[tp++];
        Node *r = binary(right ? p : p + 1);
        Node *b = node(p <= 3 ? N_LOGIC : N_BIN);
        if (t->t == T_ID) { b->op[0] = t->s->s[0] == 'i' && t->s->len == 2 ? 'i' : 'o'; } /* in / instanceof */
        else for (int i = 0; i < 4; i++) b->op[i] = t->p[i];
        b->a = l; b->b = r; l = b;
    }
    return l;
}
static Node *assign(void) {
    Node *c = binary(1);
    if (accept("?")) { Node *n = node(N_COND); n->a = c; int s = noIn; noIn = 0; n->b = assign(); noIn = s; expect(":"); n->c = assign(); return n; }
    Tok *t = cur();
    if (t->t == T_P && t->p[0] && (pis(t, "=") || (t->p[1] == '=' && !pis(t, "==") && !pis(t, "!=") && !pis(t, "<=") && !pis(t, ">=")) || pis(t, "**=") || pis(t, "<<=") || pis(t, ">>=") || pis(t, ">>>="))) {
        if (pis(t, "===") || pis(t, "!==")) return c;
        tp++; Node *n = node(N_ASSIGN);
        for (int i = 0; i < 4; i++) n->op[i] = t->p[i];
        n->a = c; n->b = assign(); return n;
    }
    return c;
}
static Node *expr(void) {
    Node *e = assign();
    if (pis(cur(), ",")) { Node *s = node(N_SEQ), **tail = &s->a; *tail = e; tail = &e->next; while (accept(",")) { *tail = assign(); tail = &(*tail)->next; } return s; }
    return e;
}
static Node *var_decl(int kind) {
    Node *v = node(N_VAR), **tail = &v->a; v->flag = (u8)kind;
    do {
        Node *d = node(N_DECL);
        if (pis(cur(), "{") || pis(cur(), "[")) { /* 分割代入は1段だけ: const {a, b} = o / const [x, y] = a */
            int arr = pis(cur(), "["); tp++;
            Node **pt = &d->c; d->flag = arr ? 2 : 1;
            while (!failed && !pis(cur(), arr ? "]" : "}")) { Node *p = node(N_ID); p->s = ident(); if (!arr && accept(":")) p->n = 1, p->b = node(N_ID), p->b->s = ident(); if (accept("=")) p->d = assign(); *pt = p; pt = &p->next; if (!accept(",")) break; }
            expect(arr ? "]" : "}");
        } else d->s = ident();
        if (accept("=")) d->a = assign();
        *tail = d; tail = &d->next;
    } while (accept(","));
    return v;
}
static Node *statement(void) {
    Tok *t = cur();
    if (accept("{")) { Node *b = node(N_BLOCK); b->a = block_body(); return b; }
    if (accept(";")) return node(N_EMPTY);
    if (t->t == T_ID) {
        if (idis(t, "var") || idis(t, "let") || idis(t, "const")) { int k = idis(t, "var") ? 0 : idis(t, "let") ? 1 : 2; tp++; Node *v = var_decl(k); semi(); return v; }
        if (idis(t, "function") || (idis(t, "async") && idis(peek(1), "function"))) {
            if (idis(t, "async")) tp++;
            tp++; Node *f = function_rest(ident(), 0); Node *s = node(N_VAR); s->flag = 9; s->a = f; return s; /* 関数宣言 */
        }
        if (accept_kw("if")) { Node *n = node(N_IF); expect("("); n->a = expr(); expect(")"); n->b = statement(); if (accept_kw("else")) n->c = statement(); return n; }
        if (accept_kw("while")) { Node *n = node(N_WHILE); expect("("); n->a = expr(); expect(")"); n->b = statement(); return n; }
        if (accept_kw("do")) { Node *n = node(N_DO); n->b = statement(); if (!accept_kw("while")) fail("構文エラー: do"); expect("("); n->a = expr(); expect(")"); accept(";"); return n; }
        if (accept_kw("for")) {
            expect("(");
            Node *init = 0; int kind = -1;
            if (idis(cur(), "var") || idis(cur(), "let") || idis(cur(), "const")) {
                kind = idis(cur(), "var") ? 0 : idis(cur(), "let") ? 1 : 2; tp++;
                if (cur()->t == T_ID && (idis(peek(1), "of") || idis(peek(1), "in"))) {
                    Node *n = node(N_FORIN); n->s = ident(); n->flag = idis(cur(), "of"); tp++; n->c = node(N_EMPTY); n->c->flag = (u8)kind;
                    n->a = expr(); expect(")"); n->b = statement(); return n;
                }
                noIn = 1; init = var_decl(kind); noIn = 0;
            } else if (!pis(cur(), ";")) {
                if (cur()->t == T_ID && (idis(peek(1), "of") || idis(peek(1), "in"))) {
                    Node *n = node(N_FORIN); n->s = ident(); n->flag = idis(cur(), "of"); tp++;
                    n->a = expr(); expect(")"); n->b = statement(); return n;
                }
                noIn = 1; init = node(N_EXPR); init->a = expr(); noIn = 0;
            }
            Node *n = node(N_FOR); n->a = init; expect(";");
            if (!pis(cur(), ";")) n->b = expr();
            expect(";");
            if (!pis(cur(), ")")) n->c = expr();
            expect(")"); n->d = statement(); n->flag = kind == 1 || kind == 2;
            return n;
        }
        if (accept_kw("return")) { Node *n = node(N_RETURN); if (!pis(cur(), ";") && !pis(cur(), "}") && !cur()->nl && cur()->t != T_EOF) n->a = expr(); semi(); return n; }
        if (accept_kw("break")) { if (cur()->t == T_ID && !cur()->nl) tp++; semi(); return node(N_BREAK); }
        if (accept_kw("continue")) { if (cur()->t == T_ID && !cur()->nl) tp++; semi(); return node(N_CONT); }
        if (accept_kw("throw")) { Node *n = node(N_THROW); n->a = expr(); semi(); return n; }
        if (accept_kw("try")) {
            Node *n = node(N_TRY); expect("{"); n->a = node(N_BLOCK); n->a->a = block_body();
            if (accept_kw("catch")) { if (accept("(")) { n->s = ident(); expect(")"); } expect("{"); n->b = node(N_BLOCK); n->b->a = block_body(); }
            if (accept_kw("finally")) { expect("{"); n->c = node(N_BLOCK); n->c->a = block_body(); }
            return n;
        }
        if (accept_kw("switch")) {
            Node *n = node(N_SWITCH); expect("("); n->a = expr(); expect(")"); expect("{");
            Node **tail = &n->b;
            while (!failed && !pis(cur(), "}") && cur()->t != T_EOF) {
                Node *c = node(N_CASE);
                if (accept_kw("case")) c->a = expr(); else if (!accept_kw("default")) { fail("構文エラー: switch"); break; }
                expect(":");
                Node **st = &c->b;
                while (!failed && !idis(cur(), "case") && !idis(cur(), "default") && !pis(cur(), "}") && cur()->t != T_EOF) { *st = statement(); if (*st) st = &(*st)->next; }
                *tail = c; tail = &c->next;
            }
            expect("}");
            return n;
        }
        if (cur()->t == T_ID && pis(peek(1), ":") ) { tp += 2; return statement(); } /* ラベル */
    }
    Node *n = node(N_EXPR); n->a = expr(); semi(); return n;
}

/* ---- 実行 ---- */
enum { C_NONE, C_RETURN, C_BREAK, C_CONT, C_THROW };
static int ctl; static JsVal ctlv;
static Env *global;
static JsObj *globalObj, *arrayProto, *objectProto;
static u64 steps;
#define STEPMAX 20000000ull
static JsVal throw_msg(const char *m) {
    JsObj *e = newobj(K_ERR);
    Prop *p = alloc(sizeof(Prop)); p->k = cstr("message"); p->v = js_cstr(m); e->props = p;
    ctl = C_THROW; ctlv = O(e);
    return js_undef();
}
static JsVal throw_str(JsStr *pre, const char *m) {
    throw_msg(m);
    obj(ctlv)->props->v = S(concat(pre, cstr(m)));
    return js_undef();
}
static Prop *find(Prop *p, const JsStr *k) { for (; p; p = p->next) if (streq(p->k, k)) return p; return 0; }
static void define(Env *e, JsStr *k, JsVal v) {
    Prop *p = find(e->vars, k);
    if (p) { p->v = v; return; }
    p = alloc(sizeof(Prop)); p->k = k; p->v = v; p->next = e->vars; e->vars = p;
}
static Prop *lookup(Env *e, const JsStr *k) {
    for (; e; e = e->up) { Prop *p = find(e->vars, k); if (p) return p; }
    return 0;
}
static void setprop(JsObj *o, JsStr *k, JsVal v) {
    Prop *p = find(o->props, k);
    if (p) { p->v = v; return; }
    p = alloc(sizeof(Prop)); p->k = k; p->v = v;
    /* 追加順を保つ (for-in / Object.keys のため) */
    Prop **t = &o->props; while (*t) t = &(*t)->next; *t = p;
}
void js_set(JsVal o, const char *k, JsVal v) { if (obj(o)) setprop(obj(o), cstr(k), v); }
void js_global(const char *name, JsVal v) { setprop(globalObj, cstr(name), v); define(global, cstr(name), v); }

static void arr_push(JsObj *a, JsVal v) {
    if (a->len == a->cap) { int nc = a->cap ? a->cap * 2 : 8; JsVal *n = alloc(sizeof(JsVal) * (u64)nc); if (a->len) mcopy(n, a->arr, sizeof(JsVal) * (u64)a->len); a->arr = n; a->cap = nc; }
    a->arr[a->len++] = v;
}
static JsObj *newarr(void) { JsObj *a = newobj(K_ARR); a->proto = arrayProto; return a; }
static int arr_index(const JsStr *k) { /* "12" なら 12、配列の添字でなければ -1 */
    if (!k->len || k->len > 9) return -1;
    int v = 0;
    for (int i = 0; i < k->len; i++) { if (k->s[i] < '0' || k->s[i] > '9') return -1; v = v * 10 + k->s[i] - '0'; }
    if (k->len > 1 && k->s[0] == '0') return -1;
    return v;
}
static JsStr *join(JsObj *a, JsStr *sep) {
    JsStr *r = newstr(0);
    for (int i = 0; i < a->len; i++) {
        if (i) r = concat(r, sep);
        if (a->arr[i].t != JS_UNDEF && a->arr[i].t != JS_NULL) r = concat(r, js_tostr(a->arr[i]));
    }
    return r;
}

/* ---- 組み込みのメソッド ---- */
#define ARG(i) (i < n ? a[i] : js_undef())
static JsStr *thisstr(JsVal self) { return js_tostr(self); }
static int clampi(double v, int len) { if (v != v) return 0; if (v < 0) { v += len; if (v < 0) v = 0; } if (v > len) v = len; return (int)v; }
static int indexof(JsStr *s, JsStr *f, int from) {
    for (int i = from < 0 ? 0 : from; i + f->len <= s->len; i++) { int j = 0; while (j < f->len && s->s[i + j] == f->s[j]) j++; if (j == f->len) return i; }
    return -1;
}
static JsVal s_charAt(JsVal self, JsVal *a, int n) { JsStr *s = thisstr(self); int i = (int)js_tonum(ARG(0)); return i >= 0 && i < s->len ? S(mkstr(s->s + i, 1)) : S(newstr(0)); }
static JsVal s_charCodeAt(JsVal self, JsVal *a, int n) { JsStr *s = thisstr(self); int i = n ? (int)js_tonum(a[0]) : 0; return i >= 0 && i < s->len ? js_num(s->s[i]) : js_num(0.0 / 0.0); }
static JsVal s_indexOf(JsVal self, JsVal *a, int n) { return js_num(indexof(thisstr(self), js_tostr(ARG(0)), n > 1 ? (int)js_tonum(a[1]) : 0)); }
static JsVal s_lastIndexOf(JsVal self, JsVal *a, int n) { JsStr *s = thisstr(self), *f = js_tostr(ARG(0)); int r = -1, i = -1; while ((i = indexof(s, f, i + 1)) >= 0) r = i; return js_num(r); }
static JsVal s_includes(JsVal self, JsVal *a, int n) { return js_bool(indexof(thisstr(self), js_tostr(ARG(0)), 0) >= 0); }
static JsVal s_startsWith(JsVal self, JsVal *a, int n) { JsStr *s = thisstr(self), *f = js_tostr(ARG(0)); return js_bool(f->len <= s->len && indexof(mkstr(s->s, f->len), f, 0) == 0); }
static JsVal s_endsWith(JsVal self, JsVal *a, int n) { JsStr *s = thisstr(self), *f = js_tostr(ARG(0)); return js_bool(f->len <= s->len && indexof(mkstr(s->s + s->len - f->len, f->len), f, 0) == 0); }
static JsVal s_slice(JsVal self, JsVal *a, int n) { JsStr *s = thisstr(self); int b = clampi(js_tonum(ARG(0)), s->len), e = n > 1 && a[1].t != JS_UNDEF ? clampi(js_tonum(a[1]), s->len) : s->len; return S(mkstr(s->s + b, e > b ? e - b : 0)); }
static JsVal s_substring(JsVal self, JsVal *a, int n) {
    JsStr *s = thisstr(self); double x = js_tonum(ARG(0)), y = n > 1 && a[1].t != JS_UNDEF ? js_tonum(a[1]) : s->len;
    int b = x != x || x < 0 ? 0 : x > s->len ? s->len : (int)x, e = y != y || y < 0 ? 0 : y > s->len ? s->len : (int)y;
    if (b > e) { int t = b; b = e; e = t; }
    return S(mkstr(s->s + b, e - b));
}
static JsVal s_substr(JsVal self, JsVal *a, int n) { JsStr *s = thisstr(self); int b = clampi(js_tonum(ARG(0)), s->len), l = n > 1 ? (int)js_tonum(a[1]) : s->len - b; if (l < 0) l = 0; if (b + l > s->len) l = s->len - b; return S(mkstr(s->s + b, l)); }
static JsVal s_case(JsVal self, int up) { JsStr *s = thisstr(self), *r = newstr(s->len); for (int i = 0; i < s->len; i++) { c16 c = s->s[i]; r->s[i] = up ? (c >= 'a' && c <= 'z' ? c - 32 : c) : (c >= 'A' && c <= 'Z' ? c + 32 : c); } return S(r); }
static JsVal s_upper(JsVal self, JsVal *a, int n) { (void)a; (void)n; return s_case(self, 1); }
static JsVal s_lower(JsVal self, JsVal *a, int n) { (void)a; (void)n; return s_case(self, 0); }
static JsVal s_trim(JsVal self, JsVal *a, int n) { (void)a; (void)n; JsStr *s = thisstr(self); int b = 0, e = s->len; while (b < e && isspace16(s->s[b])) b++; while (e > b && isspace16(s->s[e - 1])) e--; return S(mkstr(s->s + b, e - b)); }
static JsVal s_split(JsVal self, JsVal *a, int n) {
    JsStr *s = thisstr(self); JsObj *r = newarr();
    if (!n || a[0].t == JS_UNDEF) { arr_push(r, S(s)); return O(r); }
    JsStr *sep = js_tostr(a[0]);
    if (!sep->len) { for (int i = 0; i < s->len; i++) arr_push(r, S(mkstr(s->s + i, 1))); return O(r); }
    int st = 0, i;
    while ((i = indexof(s, sep, st)) >= 0) { arr_push(r, S(mkstr(s->s + st, i - st))); st = i + sep->len; }
    arr_push(r, S(mkstr(s->s + st, s->len - st)));
    return O(r);
}
static JsVal s_replace_impl(JsVal self, JsVal *a, int n, int all) {
    JsStr *s = thisstr(self), *f = js_tostr(ARG(0)), *r = newstr(0);
    int st = 0, i;
    while ((i = indexof(s, f, st)) >= 0) {
        r = concat(r, mkstr(s->s + st, i - st));
        JsVal rep = ARG(1);
        if (js_is_func(rep)) { JsVal args[2] = {S(f), js_num(i)}; rep = call(rep, js_undef(), args, 2); }
        r = concat(r, js_tostr(rep));
        st = i + f->len;
        if (!all || !f->len) break;
    }
    return S(concat(r, mkstr(s->s + st, s->len - st)));
}
static JsVal s_replace(JsVal self, JsVal *a, int n) { return s_replace_impl(self, a, n, 0); }
static JsVal s_replaceAll(JsVal self, JsVal *a, int n) { return s_replace_impl(self, a, n, 1); }
static JsVal s_repeat(JsVal self, JsVal *a, int n) { JsStr *s = thisstr(self), *r = newstr(0); int k = (int)js_tonum(ARG(0)); if (k < 0 || k > 100000 || (u64)k * s->len > 1000000) return throw_msg("RangeError: repeat"); while (k--) r = concat(r, s); return S(r); }
static JsVal s_pad(JsVal self, JsVal *a, int n, int start) {
    JsStr *s = thisstr(self), *p = n > 1 ? js_tostr(a[1]) : cstr(" "); int w = (int)js_tonum(ARG(0));
    if (w <= s->len || !p->len || w > 100000) return S(s);
    JsStr *fill = newstr(w - s->len); for (int i = 0; i < fill->len; i++) fill->s[i] = p->s[i % p->len];
    return S(start ? concat(fill, s) : concat(s, fill));
}
static JsVal s_padStart(JsVal self, JsVal *a, int n) { return s_pad(self, a, n, 1); }
static JsVal s_padEnd(JsVal self, JsVal *a, int n) { return s_pad(self, a, n, 0); }
static JsVal s_concat(JsVal self, JsVal *a, int n) { JsStr *s = thisstr(self); for (int i = 0; i < n; i++) s = concat(s, js_tostr(a[i])); return S(s); }
static JsVal s_toString(JsVal self, JsVal *a, int n) { (void)a; (void)n; return S(js_tostr(self)); }
static JsVal n_toFixed(JsVal self, JsVal *a, int n) {
    double v = js_tonum(self); int d = (int)js_tonum(ARG(0)); if (d < 0) d = 0; if (d > 20) d = 20;
    if (v != v) return js_cstr("NaN");
    int neg = v < 0; if (neg) v = -v;
    double p = dpow(10, d); double r = dfloor(v * p + 0.5);
    JsStr *ip = numstr(dfloor(r / p)), *s = neg ? concat(cstr("-"), ip) : ip;
    if (d) {
        u64 f = (u64)(r - dfloor(r / p) * p); c16 b[24]; for (int i = d - 1; i >= 0; i--) b[i] = '0' + f % 10, f /= 10;
        s = concat(concat(s, cstr(".")), mkstr(b, d));
    }
    return S(s);
}
static JsVal n_toString(JsVal self, JsVal *a, int n) {
    int radix = n ? (int)js_tonum(a[0]) : 10;
    double v = js_tonum(self);
    if (radix == 10 || radix < 2 || radix > 36 || v != dfloor(v)) return S(numstr(v));
    c16 b[70]; int k = 0, neg = v < 0; u64 u = (u64)(neg ? -v : v);
    do { int d = u % radix; b[k++] = d < 10 ? '0' + d : 'a' + d - 10; u /= radix; } while (u && k < 66);
    if (neg) b[k++] = '-';
    JsStr *r = newstr(k); for (int i = 0; i < k; i++) r->s[i] = b[k - 1 - i];
    return S(r);
}
/* 配列 */
static JsObj *thisarr(JsVal self) { JsObj *o = obj(self); return o && o->kind == K_ARR ? o : newarr(); }
static JsVal a_push(JsVal self, JsVal *a, int n) { JsObj *o = thisarr(self); for (int i = 0; i < n; i++) arr_push(o, a[i]); return js_num(o->len); }
static JsVal a_pop(JsVal self, JsVal *a, int n) { (void)a; (void)n; JsObj *o = thisarr(self); return o->len ? o->arr[--o->len] : js_undef(); }
static JsVal a_shift(JsVal self, JsVal *a, int n) { (void)a; (void)n; JsObj *o = thisarr(self); if (!o->len) return js_undef(); JsVal v = o->arr[0]; mcopy(o->arr, o->arr + 1, sizeof(JsVal) * (u64)--o->len); return v; }
static JsVal a_unshift(JsVal self, JsVal *a, int n) { JsObj *o = thisarr(self); for (int i = n - 1; i >= 0; i--) { arr_push(o, js_undef()); mcopy(o->arr + 1, o->arr, sizeof(JsVal) * (u64)(o->len - 1)); o->arr[0] = a[i]; } return js_num(o->len); }
static JsVal a_slice(JsVal self, JsVal *a, int n) { JsObj *o = thisarr(self), *r = newarr(); int b = clampi(js_tonum(ARG(0)), o->len), e = n > 1 && a[1].t != JS_UNDEF ? clampi(js_tonum(a[1]), o->len) : o->len; for (int i = b; i < e; i++) arr_push(r, o->arr[i]); return O(r); }
static JsVal a_splice(JsVal self, JsVal *a, int n) {
    JsObj *o = thisarr(self), *r = newarr(); int b = clampi(js_tonum(ARG(0)), o->len), c = n > 1 ? (int)js_tonum(a[1]) : o->len - b;
    if (c < 0) c = 0; if (b + c > o->len) c = o->len - b;
    for (int i = 0; i < c; i++) arr_push(r, o->arr[b + i]);
    JsObj *rest = newarr(); for (int i = b + c; i < o->len; i++) arr_push(rest, o->arr[i]);
    o->len = b; for (int i = 2; i < n; i++) arr_push(o, a[i]); for (int i = 0; i < rest->len; i++) arr_push(o, rest->arr[i]);
    return O(r);
}
static JsVal a_concat(JsVal self, JsVal *a, int n) {
    JsObj *o = thisarr(self), *r = newarr(); for (int i = 0; i < o->len; i++) arr_push(r, o->arr[i]);
    for (int k = 0; k < n; k++) { JsObj *x = obj(a[k]); if (x && x->kind == K_ARR) for (int i = 0; i < x->len; i++) arr_push(r, x->arr[i]); else arr_push(r, a[k]); }
    return O(r);
}
static JsVal a_join(JsVal self, JsVal *a, int n) { return S(join(thisarr(self), n && a[0].t != JS_UNDEF ? js_tostr(a[0]) : cstr(","))); }
static int strict_eq(JsVal a, JsVal b);
static JsVal a_indexOf(JsVal self, JsVal *a, int n) { JsObj *o = thisarr(self); for (int i = 0; i < o->len; i++) if (strict_eq(o->arr[i], ARG(0))) return js_num(i); return js_num(-1); }
static JsVal a_includes(JsVal self, JsVal *a, int n) { return js_bool(js_tonum(a_indexOf(self, a, n)) >= 0); }
static JsVal a_reverse(JsVal self, JsVal *a, int n) { (void)a; (void)n; JsObj *o = thisarr(self); for (int i = 0; i < o->len / 2; i++) { JsVal t = o->arr[i]; o->arr[i] = o->arr[o->len - 1 - i]; o->arr[o->len - 1 - i] = t; } return self; }
/* コールバック付き: kind 0 forEach 1 map 2 filter 3 find 4 findIndex 5 some 6 every */
static JsVal a_iter(JsVal self, JsVal *a, int n, int kind) {
    JsObj *o = thisarr(self), *r = newarr(); JsVal f = ARG(0);
    if (!js_is_func(f)) return throw_msg("TypeError: 関数ではありません");
    for (int i = 0; i < o->len; i++) {
        JsVal args[3] = {o->arr[i], js_num(i), self};
        JsVal v = call(f, n > 1 ? a[1] : js_undef(), args, 3);
        if (ctl) return js_undef();
        if (kind == 1) arr_push(r, v);
        else if (kind == 2 && js_truthy(v)) arr_push(r, o->arr[i]);
        else if (kind == 3 && js_truthy(v)) return o->arr[i];
        else if (kind == 4 && js_truthy(v)) return js_num(i);
        else if (kind == 5 && js_truthy(v)) return js_bool(1);
        else if (kind == 6 && !js_truthy(v)) return js_bool(0);
    }
    return kind == 0 || kind == 3 ? js_undef() : kind == 4 ? js_num(-1) : kind == 5 ? js_bool(0) : kind == 6 ? js_bool(1) : O(r);
}
static JsVal a_forEach(JsVal s, JsVal *a, int n) { return a_iter(s, a, n, 0); }
static JsVal a_map(JsVal s, JsVal *a, int n) { return a_iter(s, a, n, 1); }
static JsVal a_filter(JsVal s, JsVal *a, int n) { return a_iter(s, a, n, 2); }
static JsVal a_find(JsVal s, JsVal *a, int n) { return a_iter(s, a, n, 3); }
static JsVal a_findIndex(JsVal s, JsVal *a, int n) { return a_iter(s, a, n, 4); }
static JsVal a_some(JsVal s, JsVal *a, int n) { return a_iter(s, a, n, 5); }
static JsVal a_every(JsVal s, JsVal *a, int n) { return a_iter(s, a, n, 6); }
static JsVal a_reduce(JsVal self, JsVal *a, int n) {
    JsObj *o = thisarr(self); int i = 0; JsVal acc;
    if (n > 1) acc = a[1]; else { if (!o->len) return throw_msg("TypeError: reduce"); acc = o->arr[i++]; }
    for (; i < o->len; i++) { JsVal args[3] = {acc, o->arr[i], js_num(i)}; acc = call(ARG(0), js_undef(), args, 3); if (ctl) break; }
    return acc;
}
static int compare(JsVal f, JsVal x, JsVal y) {
    if (x.t == JS_UNDEF) return y.t == JS_UNDEF ? 0 : 1;
    if (y.t == JS_UNDEF) return -1;
    if (js_is_func(f)) { JsVal args[2] = {x, y}; double r = js_tonum(call(f, js_undef(), args, 2)); return r < 0 ? -1 : r > 0; }
    JsStr *a = js_tostr(x), *b = js_tostr(y);
    for (int i = 0; i < a->len && i < b->len; i++) if (a->s[i] != b->s[i]) return a->s[i] < b->s[i] ? -1 : 1;
    return a->len < b->len ? -1 : a->len > b->len;
}
static JsVal a_sort(JsVal self, JsVal *a, int n) {
    JsObj *o = thisarr(self);
    for (int i = 1; i < o->len && !ctl; i++) { JsVal v = o->arr[i]; int j = i; while (j > 0 && compare(ARG(0), o->arr[j - 1], v) > 0 && !ctl) { o->arr[j] = o->arr[j - 1]; j--; } o->arr[j] = v; }
    return self;
}
static JsVal f_call(JsVal self, JsVal *a, int n) { return call(self, ARG(0), n > 1 ? a + 1 : 0, n > 1 ? n - 1 : 0); }
static JsVal f_apply(JsVal self, JsVal *a, int n) { JsObj *l = n > 1 ? obj(a[1]) : 0; return call(self, ARG(0), l && l->kind == K_ARR ? l->arr : 0, l && l->kind == K_ARR ? l->len : 0); }
static JsVal o_hasOwn(JsVal self, JsVal *a, int n) { JsObj *o = obj(self); return js_bool(o && find(o->props, js_tostr(ARG(0)))); }

static const struct { const char *name; JsNative fn; u8 on; } methods[] = { /* on: 1 文字列 2 配列 4 数 8 関数 16 オブジェクト */
    {"charAt", s_charAt, 1}, {"charCodeAt", s_charCodeAt, 1}, {"indexOf", s_indexOf, 1}, {"lastIndexOf", s_lastIndexOf, 1},
    {"includes", s_includes, 1}, {"startsWith", s_startsWith, 1}, {"endsWith", s_endsWith, 1}, {"slice", s_slice, 1},
    {"substring", s_substring, 1}, {"substr", s_substr, 1}, {"toUpperCase", s_upper, 1}, {"toLowerCase", s_lower, 1},
    {"toLocaleUpperCase", s_upper, 1}, {"toLocaleLowerCase", s_lower, 1},
    {"trim", s_trim, 1}, {"split", s_split, 1}, {"replace", s_replace, 1}, {"replaceAll", s_replaceAll, 1}, {"repeat", s_repeat, 1},
    {"padStart", s_padStart, 1}, {"padEnd", s_padEnd, 1}, {"concat", s_concat, 1}, {"toString", s_toString, 1 | 16},
    {"toFixed", n_toFixed, 4}, {"toString", n_toString, 4}, {"toLocaleString", s_toString, 4},
    {"push", a_push, 2}, {"pop", a_pop, 2}, {"shift", a_shift, 2}, {"unshift", a_unshift, 2}, {"slice", a_slice, 2},
    {"splice", a_splice, 2}, {"concat", a_concat, 2}, {"join", a_join, 2}, {"indexOf", a_indexOf, 2}, {"includes", a_includes, 2},
    {"reverse", a_reverse, 2}, {"forEach", a_forEach, 2}, {"map", a_map, 2}, {"filter", a_filter, 2}, {"find", a_find, 2},
    {"findIndex", a_findIndex, 2}, {"some", a_some, 2}, {"every", a_every, 2}, {"reduce", a_reduce, 2}, {"sort", a_sort, 2},
    {"toString", s_toString, 2}, {"call", f_call, 8}, {"apply", f_apply, 8}, {"hasOwnProperty", o_hasOwn, 16},
};
static JsVal method(JsVal self, JsStr *k, u8 on) {
    for (unsigned i = 0; i < sizeof methods / sizeof *methods; i++)
        if ((methods[i].on & on) && js_key_is(k, methods[i].name)) return bound_native(methods[i].fn, self);
    return js_undef();
}

/* ---- 日付 ---- */
static double now_ms(void) {
    Time t;
    if (ST->rt->GetTime(&t, 0)) return 0;
    int y = t.year, m = t.month, d = t.day;
    y -= m <= 2; i64 era = (y >= 0 ? y : y - 399) / 400; int yoe = y - (int)(era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    double days = (double)(era * 146097 + doe - 719468);
    return ((days * 24 + t.hour) * 60 + t.minute) * 60000.0 + t.second * 1000.0 + t.ns / 1000000;
}
static void civil(double ms, int *y, int *mo, int *d, int *h, int *mi, int *s, int *wd) {
    double days = dfloor(ms / 86400000.0); double rem = ms - days * 86400000.0;
    *h = (int)(rem / 3600000); *mi = (int)(rem / 60000) % 60; *s = (int)(rem / 1000) % 60;
    i64 z = (i64)days + 719468; i64 era = (z >= 0 ? z : z - 146096) / 146097; int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1; *mo = mp < 10 ? mp + 3 : mp - 9; *y = (int)(yoe + era * 400) + (*mo <= 2);
    *wd = (int)(((i64)days % 7 + 11) % 7);
}
static JsStr *date_str(double ms, int what) { /* 0 全部 1 日付 2 時刻 */
    int y, mo, d, h, mi, s, wd; civil(ms, &y, &mo, &d, &h, &mi, &s, &wd);
    c16 b[40]; int n = 0;
    #define NUM2(v) b[n++] = '0' + (v) / 10 % 10, b[n++] = '0' + (v) % 10
    if (what != 2) { b[n++] = '0' + y / 1000 % 10; b[n++] = '0' + y / 100 % 10; NUM2(y % 100); b[n++] = '/'; NUM2(mo); b[n++] = '/'; NUM2(d); }
    if (what == 0) b[n++] = ' ';
    if (what != 1) { NUM2(h); b[n++] = ':'; NUM2(mi); b[n++] = ':'; NUM2(s); }
    return mkstr(b, n);
}
static JsVal d_get(JsVal self, int what) {
    JsObj *o = obj(self); if (!o) return js_num(0.0 / 0.0);
    int y, mo, d, h, mi, s, wd; civil(o->ms, &y, &mo, &d, &h, &mi, &s, &wd);
    int v[] = {y, mo - 1, d, h, mi, s, wd, (int)(o->ms - dfloor(o->ms / 1000) * 1000)};
    return js_num(v[what]);
}
static JsVal d_year(JsVal s, JsVal *a, int n) { (void)a; (void)n; return d_get(s, 0); }
static JsVal d_month(JsVal s, JsVal *a, int n) { (void)a; (void)n; return d_get(s, 1); }
static JsVal d_date(JsVal s, JsVal *a, int n) { (void)a; (void)n; return d_get(s, 2); }
static JsVal d_hours(JsVal s, JsVal *a, int n) { (void)a; (void)n; return d_get(s, 3); }
static JsVal d_minutes(JsVal s, JsVal *a, int n) { (void)a; (void)n; return d_get(s, 4); }
static JsVal d_seconds(JsVal s, JsVal *a, int n) { (void)a; (void)n; return d_get(s, 5); }
static JsVal d_day(JsVal s, JsVal *a, int n) { (void)a; (void)n; return d_get(s, 6); }
static JsVal d_ms(JsVal s, JsVal *a, int n) { (void)a; (void)n; return d_get(s, 7); }
static JsVal d_time(JsVal s, JsVal *a, int n) { (void)a; (void)n; return js_num(obj(s) ? obj(s)->ms : 0); }
static JsVal d_tstr(JsVal s, JsVal *a, int n) { (void)a; (void)n; return S(date_str(obj(s) ? obj(s)->ms : 0, 2)); }
static JsVal d_dstr(JsVal s, JsVal *a, int n) { (void)a; (void)n; return S(date_str(obj(s) ? obj(s)->ms : 0, 1)); }
static JsVal d_str(JsVal s, JsVal *a, int n) { (void)a; (void)n; return S(date_str(obj(s) ? obj(s)->ms : 0, 0)); }
static const struct { const char *name; JsNative fn; } dmethods[] = {
    {"getFullYear", d_year}, {"getMonth", d_month}, {"getDate", d_date}, {"getHours", d_hours}, {"getMinutes", d_minutes},
    {"getSeconds", d_seconds}, {"getDay", d_day}, {"getMilliseconds", d_ms}, {"getTime", d_time}, {"valueOf", d_time},
    {"toLocaleTimeString", d_tstr}, {"toLocaleDateString", d_dstr}, {"toLocaleString", d_str}, {"toString", d_str}, {"toISOString", d_str},
};

/* ---- プロパティ ---- */
static JsVal getp(JsVal o, JsStr *k) {
    switch (o.t) {
    case JS_UNDEF: case JS_NULL: { throw_str(js_key_is(k, "") ? cstr("") : concat(cstr("TypeError: undefined の "), k), " を読めません"); return js_undef(); }
    case JS_STR: {
        JsStr *s = o.p;
        if (js_key_is(k, "length")) return js_num(s->len);
        int i = arr_index(k); if (i >= 0) return i < s->len ? S(mkstr(s->s + i, 1)) : js_undef();
        return method(o, k, 1);
    }
    case JS_NUM: return method(o, k, 4);
    case JS_BOOL: return js_undef();
    }
    JsObj *x = o.p;
    if (x->kind == K_ARR) {
        if (js_key_is(k, "length")) return js_num(x->len);
        int i = arr_index(k); if (i >= 0) return i < x->len ? x->arr[i] : js_undef();
    }
    if (x->kind == K_HOST) { int found = 0; JsVal v = host_get(x->host, k, &found); if (found) return v; }
    if (x->kind == K_DATE) for (unsigned i = 0; i < sizeof dmethods / sizeof *dmethods; i++) if (js_key_is(k, dmethods[i].name)) return bound_native(dmethods[i].fn, o);
    for (JsObj *p = x; p; p = p->proto) { Prop *f = find(p->props, k); if (f) return f->v; }
    if (x->kind == K_ARR) return method(o, k, 2);
    if (x->kind == K_FUNC || x->kind == K_NATIVE) {
        if (js_key_is(k, "prototype") && x->kind == K_FUNC) { JsVal pr = js_object(); setprop(x, cstr("prototype"), pr); return pr; }
        if (js_key_is(k, "name")) return x->fn && x->fn->s ? S(x->fn->s) : S(newstr(0));
        return method(o, k, 8);
    }
    return method(o, k, 16);
}
static void putp(JsVal o, JsStr *k, JsVal v) {
    JsObj *x = obj(o);
    if (!x) { if (o.t == JS_UNDEF || o.t == JS_NULL) throw_str(concat(cstr("TypeError: undefined の "), k), " に書けません"); return; }
    if (x->kind == K_ARR) {
        int i = arr_index(k);
        if (i >= 0) { if (i > 1000000) return; while (x->len <= i) arr_push(x, js_undef()); x->arr[i] = v; return; }
        if (js_key_is(k, "length")) { int l = (int)js_tonum(v); if (l >= 0 && l <= x->len) x->len = l; return; }
    }
    if (x->kind == K_HOST && host_set(x->host, k, v)) return;
    setprop(x, k, v);
}

/* ---- 演算 ---- */
static int strict_eq(JsVal a, JsVal b) {
    if (a.t != b.t) return 0;
    switch (a.t) {
    case JS_UNDEF: case JS_NULL: return 1;
    case JS_BOOL: case JS_NUM: return a.n == b.n;
    case JS_STR: return streq(a.p, b.p);
    default: return a.p == b.p;
    }
}
static JsVal toprim(JsVal v) { if (v.t == JS_OBJ) { JsObj *o = v.p; if (o->kind == K_DATE) return js_num(o->ms); return S(js_tostr(v)); } return v; }
static int loose_eq(JsVal a, JsVal b) {
    if (a.t == b.t) return strict_eq(a, b);
    if ((a.t == JS_UNDEF || a.t == JS_NULL) && (b.t == JS_UNDEF || b.t == JS_NULL)) return 1;
    if (a.t == JS_UNDEF || a.t == JS_NULL || b.t == JS_UNDEF || b.t == JS_NULL) return 0;
    if (a.t == JS_OBJ) return loose_eq(toprim(a), b);
    if (b.t == JS_OBJ) return loose_eq(a, toprim(b));
    return js_tonum(a) == js_tonum(b);
}
static int cmpstr(JsStr *a, JsStr *b) { for (int i = 0; i < a->len && i < b->len; i++) if (a->s[i] != b->s[i]) return a->s[i] < b->s[i] ? -1 : 1; return a->len - b->len; }
static i64 toi32(double v) { if (v != v || v > 9e18 || v < -9e18) return 0; return (i64)(int)(u32)(u64)(i64)v; }
static JsVal binop(const char *op, JsVal a, JsVal b) {
    if (op[0] == '+' && !op[1]) {
        a = toprim(a); b = toprim(b);
        if (a.t == JS_STR || b.t == JS_STR) return S(concat(js_tostr(a), js_tostr(b)));
        return js_num(js_tonum(a) + js_tonum(b));
    }
    if (op[0] == '=' && op[1] == '=') return js_bool(op[2] ? strict_eq(a, b) : loose_eq(a, b));
    if (op[0] == '!' && op[1] == '=') return js_bool(!(op[2] ? strict_eq(a, b) : loose_eq(a, b)));
    if (op[0] == '<' || op[0] == '>') {
        if (op[1] == op[0]) { /* シフト */
            i64 x = toi32(js_tonum(a)); int s = (int)toi32(js_tonum(b)) & 31;
            if (op[0] == '<') return js_num((double)(int)((u32)x << s));
            if (op[2] == '>') return js_num((double)((u32)x >> s));
            return js_num((double)((int)x >> s));
        }
        a = toprim(a); b = toprim(b);
        int r;
        if (a.t == JS_STR && b.t == JS_STR) r = cmpstr(a.p, b.p);
        else { double x = js_tonum(a), y = js_tonum(b); if (x != x || y != y) return js_bool(0); r = x < y ? -1 : x > y; }
        if (op[0] == '<') return js_bool(op[1] == '=' ? r <= 0 : r < 0);
        return js_bool(op[1] == '=' ? r >= 0 : r > 0);
    }
    if (op[0] == 'i') { /* in */
        JsObj *o = obj(b); if (!o) return throw_msg("TypeError: in");
        JsStr *k = js_tostr(a); if (o->kind == K_ARR) { int i = arr_index(k); if (i >= 0) return js_bool(i < o->len); }
        for (; o; o = o->proto) if (find(o->props, k)) return js_bool(1);
        return js_bool(0);
    }
    if (op[0] == 'o') { /* instanceof */
        JsObj *x = obj(a), *f = obj(b); if (!x || !f) return js_bool(0);
        Prop *pp = find(f->props, cstr("prototype")); JsObj *pr = pp ? obj(pp->v) : 0;
        if (f->kind == K_NATIVE) { if (f->nat == 0) return js_bool(0); }
        for (JsObj *p = x->proto; p; p = p->proto) if (p == pr) return js_bool(1);
        if (pr == arrayProto) return js_bool(x->kind == K_ARR);
        return js_bool(0);
    }
    double x = js_tonum(a), y = js_tonum(b);
    switch (op[0]) {
    case '-': return js_num(x - y);
    case '*': return js_num(op[1] == '*' ? dpow(x, y) : x * y);
    case '/': return js_num(x / y);
    case '%': { if (y == 0 || x != x || y != y) return js_num(0.0 / 0.0); double q = x / y; q = q < 0 ? -dfloor(-q) : dfloor(q); return js_num(x - q * y); }
    case '&': return js_num((double)(int)(toi32(x) & toi32(y)));
    case '|': return js_num((double)(int)(toi32(x) | toi32(y)));
    case '^': return js_num((double)(int)(toi32(x) ^ toi32(y)));
    }
    return js_undef();
}

/* ---- 評価 ---- */
static JsVal eval(Node *n, Env *e);
static void exec_list(Node *s, Env *e);
static JsVal make_func(Node *f, Env *e) { JsObj *o = newobj(K_FUNC); o->fn = f; o->env = e; return O(o); }
static void hoist(Node *s, Env *e) { /* 関数宣言を先に定義する */
    for (; s; s = s->next) if (s->t == N_VAR && s->flag == 9) define(e, s->a->s, make_func(s->a, e));
}
static JsVal self_of(Env *e) { for (; e; e = e->up) if (e->hasSelf) return e->self; return js_undef(); }
static JsVal call(JsVal f, JsVal self, JsVal *args, int n) {
    JsObj *o = obj(f);
    if (!o || (o->kind != K_FUNC && o->kind != K_NATIVE)) return throw_msg("TypeError: 関数ではありません");
    if (o->kind == K_NATIVE) return o->nat(o->bound.t ? o->bound : self, args, n);
    static int depth;
    if (depth > 200) return throw_msg("RangeError: 呼び出しが深すぎます");
    Node *fn = o->fn;
    Env *env = alloc(sizeof(Env)); env->up = o->env;
    if (!(fn->flag & 1)) { env->hasSelf = 1; env->self = self; } /* アロー関数は外の this */
    int i = 0;
    for (Node *p = fn->a; p; p = p->next, i++) {
        if (p->flag) { JsObj *rest = newarr(); for (int k = i; k < n; k++) arr_push(rest, args[k]); define(env, p->s, O(rest)); break; }
        JsVal v = i < n ? args[i] : js_undef();
        if (v.t == JS_UNDEF && p->b) v = eval(p->b, env);
        define(env, p->s, v);
    }
    if (!(fn->flag & 1)) { JsObj *ar = newarr(); for (int k = 0; k < n; k++) arr_push(ar, args[k]); define(env, cstr("arguments"), O(ar)); }
    depth++;
    JsVal r = js_undef();
    if (fn->flag == 3) r = eval(fn->b, env);
    else {
        hoist(fn->b->a, env);
        exec_list(fn->b->a, env);
        if (ctl == C_RETURN) { r = ctlv; ctl = C_NONE; }
        else if (ctl == C_BREAK || ctl == C_CONT) ctl = C_NONE;
    }
    depth--;
    return r;
}
static int collect_args(Node *a, Env *e, JsVal *out, int max) {
    int n = 0;
    for (; a && n < max; a = a->next) {
        if (a->t == N_SPREAD) { JsVal v = eval(a->a, e); JsObj *o = obj(v); if (o && o->kind == K_ARR) for (int i = 0; i < o->len && n < max; i++) out[n++] = o->arr[i]; }
        else out[n++] = eval(a, e);
        if (ctl) return n;
    }
    return n;
}
/* 代入先 */
static void assign_to(Node *t, JsVal v, Env *e) {
    if (t->t == N_ID) {
        Prop *p = lookup(e, t->s);
        if (p) p->v = v; else { define(global, t->s, v); setprop(globalObj, t->s, v); }
    } else if (t->t == N_MEMBER) { JsVal o = eval(t->a, e); if (!ctl) putp(o, t->s, v); }
    else if (t->t == N_INDEX) { JsVal o = eval(t->a, e); if (ctl) return; JsVal k = eval(t->b, e); if (!ctl) putp(o, js_tostr(k), v); }
    else throw_msg("SyntaxError: 代入できません");
}
static JsVal eval(Node *n, Env *e) {
    if (ctl || !n) return js_undef();
    if (++steps > STEPMAX) return throw_msg("スクリプトの実行が長すぎます");
    switch (n->t) {
    case N_NUM: return js_num(n->n);
    case N_STR: return S(n->s);
    case N_THIS: return self_of(e);
    case N_ID: {
        Prop *p = lookup(e, n->s);
        if (p) return p->v;
        if (js_key_is(n->s, "undefined")) return js_undef();
        if (js_key_is(n->s, "null")) return jnull();
        if (js_key_is(n->s, "true")) return js_bool(1);
        if (js_key_is(n->s, "false")) return js_bool(0);
        if (js_key_is(n->s, "NaN")) return js_num(0.0 / 0.0);
        if (js_key_is(n->s, "Infinity")) return js_num(1.0 / 0.0);
        Prop *g = find(globalObj->props, n->s); if (g) return g->v;
        return throw_str(n->s, " is not defined");
    }
    case N_ARR: {
        JsObj *a = newarr();
        for (Node *x = n->a; x && !ctl; x = x->next) {
            if (x->t == N_SPREAD) { JsVal v = eval(x->a, e); JsObj *o = obj(v); if (o && o->kind == K_ARR) for (int i = 0; i < o->len; i++) arr_push(a, o->arr[i]); else if (v.t == JS_STR) { JsStr *s = v.p; for (int i = 0; i < s->len; i++) arr_push(a, S(mkstr(s->s + i, 1))); } }
            else arr_push(a, eval(x, e));
        }
        return O(a);
    }
    case N_OBJ: {
        JsObj *o = newobj(K_OBJ); o->proto = objectProto;
        for (Node *p = n->a; p && !ctl; p = p->next) {
            if (p->flag == 2) { JsObj *src = obj(eval(p->b, e)); if (src) for (Prop *q = src->props; q; q = q->next) setprop(o, q->k, q->v); continue; }
            JsStr *k = p->c ? js_tostr(eval(p->c, e)) : p->s;
            setprop(o, k, eval(p->b, e));
        }
        return O(o);
    }
    case N_FUNC: return make_func(n, e);
    case N_MEMBER: { JsVal o = eval(n->a, e); if (ctl) return o; return getp(o, n->s); }
    case N_INDEX: { JsVal o = eval(n->a, e); if (ctl) return o; JsVal k = eval(n->b, e); if (ctl) return k; return getp(o, js_tostr(k)); }
    case N_CALL: {
        JsVal self = js_undef(), f;
        if (n->a->t == N_MEMBER || n->a->t == N_INDEX) {
            self = eval(n->a->a, e); if (ctl) return self;
            JsStr *k = n->a->t == N_MEMBER ? n->a->s : js_tostr(eval(n->a->b, e));
            if (ctl) return js_undef();
            f = getp(self, k);
            if (!ctl && !js_is_func(f)) return throw_str(concat(k, cstr("")), " is not a function");
        } else { f = eval(n->a, e); if (!ctl && !js_is_func(f)) return throw_msg("TypeError: 関数ではありません"); }
        if (ctl) return js_undef();
        JsVal args[32]; int argc = collect_args(n->b, e, args, 32);
        if (ctl) return js_undef();
        return call(f, self, args, argc);
    }
    case N_NEW: {
        JsVal f = eval(n->a, e); if (ctl) return f;
        JsVal args[16]; int argc = collect_args(n->b, e, args, 16);
        JsObj *fo = obj(f);
        if (!fo) return throw_msg("TypeError: new できません");
        if (fo->kind == K_NATIVE) return fo->nat(js_bool(1) /* new の印 */, args, argc);
        JsObj *o = newobj(K_OBJ); o->proto = obj(getp(f, cstr("prototype")));
        JsVal r = call(f, O(o), args, argc);
        return r.t == JS_OBJ ? r : O(o);
    }
    case N_UNARY: {
        if (n->op[0] == 't') { /* typeof: 未定義の名前でもエラーにしない */
            if (n->a->t == N_ID && !lookup(e, n->a->s) && !find(globalObj->props, n->a->s) && !js_key_is(n->a->s, "null") && !js_key_is(n->a->s, "true") && !js_key_is(n->a->s, "false") && !js_key_is(n->a->s, "NaN") && !js_key_is(n->a->s, "Infinity")) return js_cstr("undefined");
            JsVal v = eval(n->a, e);
            const char *t = v.t == JS_UNDEF ? "undefined" : v.t == JS_NULL ? "object" : v.t == JS_BOOL ? "boolean" : v.t == JS_NUM ? "number" : v.t == JS_STR ? "string" : js_is_func(v) ? "function" : "object";
            return js_cstr(t);
        }
        if (n->op[0] == 'd') { /* delete */
            if (n->a->t == N_MEMBER || n->a->t == N_INDEX) {
                JsObj *o = obj(eval(n->a->a, e)); JsStr *k = n->a->t == N_MEMBER ? n->a->s : js_tostr(eval(n->a->b, e));
                if (o) for (Prop **p = &o->props; *p; p = &(*p)->next) if (streq((*p)->k, k)) { *p = (*p)->next; break; }
            }
            return js_bool(1);
        }
        JsVal v = eval(n->a, e);
        switch (n->op[0]) {
        case '!': return js_bool(!js_truthy(v));
        case '-': return js_num(-js_tonum(v));
        case '+': return js_num(js_tonum(v));
        case '~': return js_num((double)(int)~toi32(js_tonum(v)));
        case 'v': return js_undef();
        default: return v; /* await */
        }
    }
    case N_BIN: { JsVal a = eval(n->a, e); if (ctl) return a; JsVal b = eval(n->b, e); if (ctl) return b; return binop(n->op, a, b); }
    case N_LOGIC: {
        JsVal a = eval(n->a, e); if (ctl) return a;
        if (n->op[0] == '&') return js_truthy(a) ? eval(n->b, e) : a;
        if (n->op[0] == '|') return js_truthy(a) ? a : eval(n->b, e);
        return a.t == JS_UNDEF || a.t == JS_NULL ? eval(n->b, e) : a; /* ?? */
    }
    case N_COND: { JsVal c = eval(n->a, e); if (ctl) return c; return eval(js_truthy(c) ? n->b : n->c, e); }
    case N_SEQ: { JsVal v = js_undef(); for (Node *x = n->a; x && !ctl; x = x->next) v = eval(x, e); return v; }
    case N_ASSIGN: {
        JsVal v;
        if (n->op[0] == '=' && !n->op[1]) v = eval(n->b, e);
        else {
            JsVal old = eval(n->a, e); if (ctl) return old;
            if ((n->op[0] == '&' && n->op[1] == '&') || (n->op[0] == '|' && n->op[1] == '|') || (n->op[0] == '?' && n->op[1] == '?')) {
                int keep = n->op[0] == '&' ? !js_truthy(old) : n->op[0] == '|' ? js_truthy(old) : !(old.t == JS_UNDEF || old.t == JS_NULL);
                if (keep) return old;
                v = eval(n->b, e);
            } else {
                char op[5] = {0}; int L = 0; while (n->op[L] && n->op[L] != '=') op[L] = n->op[L], L++;
                JsVal r = eval(n->b, e); if (ctl) return r;
                v = binop(op, old, r);
            }
        }
        if (ctl) return v;
        assign_to(n->a, v, e);
        return v;
    }
    case N_UPDATE: {
        JsVal old = eval(n->a, e); if (ctl) return old;
        double o = js_tonum(old), v = n->op[0] == '+' ? o + 1 : o - 1;
        assign_to(n->a, js_num(v), e);
        return js_num(n->flag ? v : o);
    }
    default: return throw_msg("SyntaxError: 式ではありません");
    }
}
static void declare(Node *v, Env *e, Env *fnEnv) {
    for (Node *d = v->a; d && !ctl; d = d->next) {
        JsVal val = d->a ? eval(d->a, e) : js_undef();
        if (ctl) return;
        Env *target = v->flag == 0 ? fnEnv : e;
        if (!d->flag) { if (v->flag == 0 && !d->a && find(target->vars, d->s)) continue; define(target, d->s, val); continue; }
        int i = 0;
        for (Node *p = d->c; p; p = p->next, i++) { /* 分割代入 */
            JsVal x = d->flag == 2 ? getp(val, numstr(i)) : getp(val, p->s);
            if (ctl) return;
            if (x.t == JS_UNDEF && p->d) x = eval(p->d, e);
            define(target, p->b ? p->b->s : p->s, x);
        }
    }
}
static Env *fnenv_of(Env *e) { for (Env *x = e; x; x = x->up) if (x->hasSelf || !x->up) return x; return e; }
static void exec(Node *s, Env *e) {
    if (ctl || !s) return;
    if (++steps > STEPMAX) { throw_msg("スクリプトの実行が長すぎます"); return; }
    switch (s->t) {
    case N_EXPR: eval(s->a, e); return;
    case N_VAR: if (s->flag != 9) declare(s, e, fnenv_of(e)); return;
    case N_EMPTY: return;
    case N_BLOCK: { Env *b = alloc(sizeof(Env)); b->up = e; hoist(s->a, b); exec_list(s->a, b); return; }
    case N_IF: { JsVal c = eval(s->a, e); if (ctl) return; exec(js_truthy(c) ? s->b : s->c, e); return; }
    case N_WHILE: case N_DO:
        for (int first = 1;; first = 0) {
            if (!(s->t == N_DO && first)) { JsVal c = eval(s->a, e); if (ctl || !js_truthy(c)) return; }
            exec(s->b, e);
            if (ctl == C_BREAK) { ctl = C_NONE; return; }
            if (ctl == C_CONT) ctl = C_NONE;
            if (ctl) return;
        }
    case N_FOR: {
        Env *le = alloc(sizeof(Env)); le->up = e;
        if (s->a) { if (s->a->t == N_VAR) declare(s->a, le, s->a->flag == 0 ? fnenv_of(e) : le); else exec(s->a, le); }
        for (;;) {
            if (ctl) return;
            Env *it = le;
            if (s->flag) { it = alloc(sizeof(Env)); it->up = e; for (Prop *p = le->vars; p; p = p->next) define(it, p->k, p->v); } /* let は回ごとに別 */
            if (s->b) { JsVal c = eval(s->b, it); if (ctl || !js_truthy(c)) return; }
            exec(s->d, it);
            if (ctl == C_BREAK) { ctl = C_NONE; return; }
            if (ctl == C_CONT) ctl = C_NONE;
            if (ctl) return;
            if (s->flag) for (Prop *p = it->vars; p; p = p->next) { Prop *q = find(le->vars, p->k); if (q) q->v = p->v; }
            if (s->c) eval(s->c, le);
        }
    }
    case N_FORIN: {
        JsVal src = eval(s->a, e); if (ctl) return;
        JsObj *keys = newarr();
        JsObj *o = obj(src);
        if (s->flag) { /* of: 値 */
            if (o && o->kind == K_ARR) for (int i = 0; i < o->len; i++) arr_push(keys, o->arr[i]);
            else if (src.t == JS_STR) { JsStr *t = src.p; for (int i = 0; i < t->len; i++) arr_push(keys, S(mkstr(t->s + i, 1))); }
            else { throw_msg("TypeError: 繰り返せません"); return; }
        } else if (o) { /* in: 名前 */
            if (o->kind == K_ARR) for (int i = 0; i < o->len; i++) arr_push(keys, S(numstr(i)));
            for (Prop *p = o->props; p; p = p->next) arr_push(keys, S(p->k));
        }
        for (int i = 0; i < keys->len; i++) {
            Env *it = alloc(sizeof(Env)); it->up = e;
            if (s->c) define(s->c->flag == 0 ? fnenv_of(e) : it, s->s, keys->arr[i]);
            else { Node id = {0}; id.t = N_ID; id.s = s->s; assign_to(&id, keys->arr[i], e); }
            exec(s->b, it);
            if (ctl == C_BREAK) { ctl = C_NONE; return; }
            if (ctl == C_CONT) ctl = C_NONE;
            if (ctl) return;
        }
        return;
    }
    case N_RETURN: { JsVal v = s->a ? eval(s->a, e) : js_undef(); if (ctl) return; ctl = C_RETURN; ctlv = v; return; }
    case N_BREAK: ctl = C_BREAK; return;
    case N_CONT: ctl = C_CONT; return;
    case N_THROW: { JsVal v = eval(s->a, e); if (ctl) return; ctl = C_THROW; ctlv = v; return; }
    case N_TRY: {
        exec(s->a, e);
        if (ctl == C_THROW && s->b) {
            JsVal ex = ctlv; ctl = C_NONE;
            Env *c = alloc(sizeof(Env)); c->up = e; if (s->s) define(c, s->s, ex);
            exec(s->b, c);
        }
        if (s->c) { int sc = ctl; JsVal sv = ctlv; ctl = C_NONE; exec(s->c, e); if (!ctl) ctl = sc, ctlv = sv; }
        return;
    }
    case N_SWITCH: {
        JsVal v = eval(s->a, e); if (ctl) return;
        Env *b = alloc(sizeof(Env)); b->up = e;
        Node *start = 0;
        for (Node *c = s->b; c && !start; c = c->next) if (c->a) { JsVal x = eval(c->a, b); if (ctl) return; if (strict_eq(v, x)) start = c; }
        if (!start) for (Node *c = s->b; c; c = c->next) if (!c->a) start = c;
        for (Node *c = start; c && !ctl; c = c->next) exec_list(c->b, b);
        if (ctl == C_BREAK) ctl = C_NONE;
        return;
    }
    default: eval(s, e); return;
    }
}
static void exec_list(Node *s, Env *e) { for (; s && !ctl; s = s->next) exec(s, e); }

/* ---- 組み込みの関数とオブジェクト ---- */
static JsVal g_parseInt(JsVal self, JsVal *a, int n) {
    (void)self; JsStr *s = js_tostr(ARG(0)); int radix = n > 1 ? (int)js_tonum(a[1]) : 10, i = 0, neg = 0; double v = 0; int any = 0;
    while (i < s->len && isspace16(s->s[i])) i++;
    if (i < s->len && (s->s[i] == '-' || s->s[i] == '+')) neg = s->s[i++] == '-';
    if ((radix == 16 || radix == 0) && i + 1 < s->len && s->s[i] == '0' && (s->s[i + 1] | 32) == 'x') i += 2, radix = 16;
    if (radix == 0) radix = 10;
    for (; i < s->len; i++) { c16 c = s->s[i] | 32; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : 99; if (d >= radix) break; v = v * radix + d; any = 1; }
    return js_num(!any ? 0.0 / 0.0 : neg ? -v : v);
}
static JsVal g_parseFloat(JsVal self, JsVal *a, int n) {
    (void)self; JsStr *s = js_tostr(ARG(0)); int i = 0; while (i < s->len && isspace16(s->s[i])) i++;
    int st = i; if (i < s->len && (s->s[i] == '-' || s->s[i] == '+')) i++;
    while (i < s->len && ((s->s[i] >= '0' && s->s[i] <= '9') || s->s[i] == '.')) i++;
    if (i < s->len && (s->s[i] | 32) == 'e') { int j = i + 1; if (j < s->len && (s->s[j] == '-' || s->s[j] == '+')) j++; if (j < s->len && s->s[j] >= '0' && s->s[j] <= '9') { i = j; while (i < s->len && s->s[i] >= '0' && s->s[i] <= '9') i++; } }
    return js_num(strnum(mkstr(s->s + st, i - st)));
}
static JsVal g_isNaN(JsVal self, JsVal *a, int n) { (void)self; double v = js_tonum(ARG(0)); return js_bool(v != v); }
static JsVal g_isFinite(JsVal self, JsVal *a, int n) { (void)self; double v = js_tonum(ARG(0)); return js_bool(v == v && v != 1.0 / 0.0 && v != -1.0 / 0.0); }
static JsVal g_Number(JsVal self, JsVal *a, int n) { (void)self; return js_num(n ? js_tonum(a[0]) : 0); }
static JsVal g_String(JsVal self, JsVal *a, int n) { (void)self; return S(n ? js_tostr(a[0]) : newstr(0)); }
static JsVal g_Boolean(JsVal self, JsVal *a, int n) { (void)self; return js_bool(js_truthy(ARG(0))); }
static JsVal g_Array(JsVal self, JsVal *a, int n) { (void)self; JsObj *r = newarr(); if (n == 1 && a[0].t == JS_NUM) { for (int i = 0; i < (int)a[0].n && i < 1000000; i++) arr_push(r, js_undef()); } else for (int i = 0; i < n; i++) arr_push(r, a[i]); return O(r); }
static JsVal g_Object(JsVal self, JsVal *a, int n) { (void)self; (void)a; (void)n; JsObj *o = newobj(K_OBJ); o->proto = objectProto; return O(o); }
static JsVal g_isArray(JsVal self, JsVal *a, int n) { (void)self; JsObj *o = obj(ARG(0)); return js_bool(o && o->kind == K_ARR); }
static JsVal g_from(JsVal self, JsVal *a, int n) {
    (void)self; JsObj *r = newarr(); JsVal src = ARG(0); JsObj *o = obj(src);
    if (o && o->kind == K_ARR) for (int i = 0; i < o->len; i++) arr_push(r, o->arr[i]);
    else if (src.t == JS_STR) { JsStr *s = src.p; for (int i = 0; i < s->len; i++) arr_push(r, S(mkstr(s->s + i, 1))); }
    else if (o) { int len = (int)js_tonum(getp(src, cstr("length"))); for (int i = 0; i < len && i < 100000; i++) arr_push(r, getp(src, numstr(i))); }
    if (n > 1 && js_is_func(a[1])) for (int i = 0; i < r->len && !ctl; i++) { JsVal args[2] = {r->arr[i], js_num(i)}; r->arr[i] = call(a[1], js_undef(), args, 2); }
    return O(r);
}
static JsVal g_keys_impl(JsVal *a, int n, int what) {
    JsObj *r = newarr(), *o = obj(ARG(0)); if (!o) return O(r);
    if (o->kind == K_ARR) for (int i = 0; i < o->len; i++) arr_push(r, what == 0 ? S(numstr(i)) : o->arr[i]);
    for (Prop *p = o->props; p; p = p->next) {
        if (what == 2) { JsObj *pair = newarr(); arr_push(pair, S(p->k)); arr_push(pair, p->v); arr_push(r, O(pair)); }
        else arr_push(r, what == 0 ? S(p->k) : p->v);
    }
    return O(r);
}
static JsVal g_keys(JsVal self, JsVal *a, int n) { (void)self; return g_keys_impl(a, n, 0); }
static JsVal g_values(JsVal self, JsVal *a, int n) { (void)self; return g_keys_impl(a, n, 1); }
static JsVal g_entries(JsVal self, JsVal *a, int n) { (void)self; return g_keys_impl(a, n, 2); }
static JsVal g_assign(JsVal self, JsVal *a, int n) { (void)self; JsObj *t = obj(ARG(0)); if (!t) return ARG(0); for (int i = 1; i < n; i++) { JsObj *s = obj(a[i]); if (s) for (Prop *p = s->props; p; p = p->next) setprop(t, p->k, p->v); } return a[0]; }
static JsVal g_fromCharCode(JsVal self, JsVal *a, int n) { (void)self; JsStr *s = newstr(n); for (int i = 0; i < n; i++) s->s[i] = (c16)js_tonum(a[i]); return S(s); }
static JsVal g_Date(JsVal self, JsVal *a, int n) {
    JsObj *d = newobj(K_DATE);
    d->ms = n && a[0].t == JS_NUM ? a[0].n : now_ms();
    if (n >= 3) { /* new Date(年, 月, 日, 時, 分, 秒) */
        int y = (int)js_tonum(a[0]), m = (int)js_tonum(a[1]) + 1, dd = (int)js_tonum(a[2]);
        y -= m <= 2; i64 era = (y >= 0 ? y : y - 399) / 400; int yoe = y - (int)(era * 400);
        int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        d->ms = (double)(era * 146097 + doe - 719468) * 86400000.0 + (n > 3 ? js_tonum(a[3]) * 3600000 : 0) + (n > 4 ? js_tonum(a[4]) * 60000 : 0) + (n > 5 ? js_tonum(a[5]) * 1000 : 0);
    }
    if (self.t != JS_BOOL) return S(date_str(d->ms, 0)); /* new なしで呼ぶと文字列 */
    return O(d);
}
static JsVal g_now(JsVal self, JsVal *a, int n) { (void)self; (void)a; (void)n; return js_num(now_ms()); }
static JsVal g_Error(JsVal self, JsVal *a, int n) { (void)self; JsObj *e = newobj(K_ERR); setprop(e, cstr("message"), n ? S(js_tostr(a[0])) : S(newstr(0))); return O(e); }
/* Math */
static JsVal m1(JsVal *a, int n, int f) {
    double x = js_tonum(ARG(0));
    switch (f) {
    case 0: return js_num(dfloor(x));
    case 1: return js_num(-dfloor(-x));
    case 2: return js_num(dfloor(x + 0.5));
    case 3: return js_num(x < 0 ? -x : x);
    case 4: return js_num(dsqrt(x));
    case 5: return js_num(dsin(x));
    case 6: return js_num(dsin(x + 1.5707963267948966));
    case 7: return js_num(dlog(x));
    case 8: return js_num(dexp(x));
    case 9: return js_num(x < 0 ? -dfloor(-x) : dfloor(x));
    case 10: return js_num(x > 0 ? 1 : x < 0 ? -1 : x);
    case 11: return js_num(dsin(x) / dsin(x + 1.5707963267948966));
    }
    return js_undef();
}
static JsVal m_floor(JsVal s, JsVal *a, int n) { (void)s; return m1(a, n, 0); }
static JsVal m_ceil(JsVal s, JsVal *a, int n) { (void)s; return m1(a, n, 1); }
static JsVal m_round(JsVal s, JsVal *a, int n) { (void)s; return m1(a, n, 2); }
static JsVal m_abs(JsVal s, JsVal *a, int n) { (void)s; return m1(a, n, 3); }
static JsVal m_sqrt(JsVal s, JsVal *a, int n) { (void)s; return m1(a, n, 4); }
static JsVal m_sin(JsVal s, JsVal *a, int n) { (void)s; return m1(a, n, 5); }
static JsVal m_cos(JsVal s, JsVal *a, int n) { (void)s; return m1(a, n, 6); }
static JsVal m_log(JsVal s, JsVal *a, int n) { (void)s; return m1(a, n, 7); }
static JsVal m_exp(JsVal s, JsVal *a, int n) { (void)s; return m1(a, n, 8); }
static JsVal m_trunc(JsVal s, JsVal *a, int n) { (void)s; return m1(a, n, 9); }
static JsVal m_sign(JsVal s, JsVal *a, int n) { (void)s; return m1(a, n, 10); }
static JsVal m_tan(JsVal s, JsVal *a, int n) { (void)s; return m1(a, n, 11); }
static JsVal m_pow(JsVal s, JsVal *a, int n) { (void)s; return js_num(dpow(js_tonum(ARG(0)), js_tonum(ARG(1)))); }
static JsVal m_random(JsVal s, JsVal *a, int n) { (void)s; (void)a; (void)n; return js_num(drand()); }
static JsVal m_minmax(JsVal *a, int n, int mx) {
    double r = mx ? -1.0 / 0.0 : 1.0 / 0.0;
    for (int i = 0; i < n; i++) { double v = js_tonum(a[i]); if (v != v) return js_num(v); if (mx ? v > r : v < r) r = v; }
    return js_num(r);
}
static JsVal m_min(JsVal s, JsVal *a, int n) { (void)s; return m_minmax(a, n, 0); }
static JsVal m_max(JsVal s, JsVal *a, int n) { (void)s; return m_minmax(a, n, 1); }
/* JSON.stringify (簡易) */
static JsStr *json(JsVal v, int depth) {
    if (depth > 20) return cstr("null");
    if (v.t == JS_STR) {
        JsStr *s = v.p, *r = cstr("\"");
        for (int i = 0; i < s->len; i++) {
            c16 c = s->s[i];
            if (c == '"' || c == '\\') r = concat(r, mkstr((c16[]){'\\', c}, 2));
            else if (c == '\n') r = concat(r, cstr("\\n"));
            else r = concat(r, mkstr(&c, 1));
        }
        return concat(r, cstr("\""));
    }
    if (v.t == JS_NUM) return v.n == v.n ? numstr(v.n) : cstr("null");
    if (v.t != JS_OBJ) return v.t == JS_BOOL ? cstr(v.n ? "true" : "false") : cstr("null");
    JsObj *o = v.p;
    if (o->kind == K_ARR) { JsStr *r = cstr("["); for (int i = 0; i < o->len; i++) { if (i) r = concat(r, cstr(",")); r = concat(r, json(o->arr[i], depth + 1)); } return concat(r, cstr("]")); }
    if (js_is_func(v)) return cstr("null");
    JsStr *r = cstr("{"); int first = 1;
    for (Prop *p = o->props; p; p = p->next) {
        if (p->v.t == JS_UNDEF || js_is_func(p->v)) continue;
        if (!first) r = concat(r, cstr(","));
        first = 0; r = concat(concat(concat(r, json(S(p->k), 0)), cstr(":")), json(p->v, depth + 1));
    }
    return concat(r, cstr("}"));
}
static JsVal g_stringify(JsVal s, JsVal *a, int n) { (void)s; return S(json(ARG(0), 0)); }
static JsVal g_jsonparse(JsVal s, JsVal *a, int n) { /* 式として評価する (簡易) */
    (void)s; JsStr *src = js_tostr(ARG(0));
    int save = tp, sn = ntok; Tok *st = toks;
    JsVal r = js_undef();
    if (lex(src->s, src->len)) { tp = 0; Node *x = assign(); if (!failed) r = eval(x, global); }
    toks = st; ntok = sn; tp = save;
    if (failed) { failed = 0; return throw_msg("SyntaxError: JSON"); }
    return r;
}

static JsObj *mkns(const char *name) { JsObj *o = newobj(K_OBJ); setprop(globalObj, cstr(name), O(o)); define(global, cstr(name), O(o)); return o; }
static void fnprop(JsObj *o, const char *name, JsNative f) { setprop(o, cstr(name), js_native(f)); }

void js_reset(void *mem, u64 size) {
    heap = mem; heapSize = size; heapUsed = 0; oom = 0;
    ctl = C_NONE; failed = 0; steps = 0;
    global = alloc(sizeof(Env)); global->hasSelf = 1;
    globalObj = newobj(K_OBJ);
    global->self = O(globalObj);
    objectProto = newobj(K_OBJ); arrayProto = newobj(K_OBJ);
    rngState ^= (u64)now_ms() * 2654435761ull + 1;
    js_global("window", O(globalObj)); js_global("globalThis", O(globalObj)); js_global("self", O(globalObj));
    js_global("parseInt", js_native(g_parseInt)); js_global("parseFloat", js_native(g_parseFloat));
    js_global("isNaN", js_native(g_isNaN)); js_global("isFinite", js_native(g_isFinite));
    js_global("Number", js_native(g_Number)); js_global("String", js_native(g_String)); js_global("Boolean", js_native(g_Boolean));
    js_global("Error", js_native(g_Error)); js_global("TypeError", js_native(g_Error)); js_global("RangeError", js_native(g_Error));
    JsVal arr = js_native(g_Array); js_global("Array", arr); fnprop(obj(arr), "isArray", g_isArray); fnprop(obj(arr), "from", g_from);
    setprop(obj(arr), cstr("prototype"), O(arrayProto));
    JsVal ob = js_native(g_Object); js_global("Object", ob); fnprop(obj(ob), "keys", g_keys); fnprop(obj(ob), "values", g_values);
    fnprop(obj(ob), "entries", g_entries); fnprop(obj(ob), "assign", g_assign);
    setprop(obj(ob), cstr("prototype"), O(objectProto));
    fnprop(obj(find(globalObj->props, cstr("String"))->v), "fromCharCode", g_fromCharCode);
    JsVal date = js_native(g_Date); js_global("Date", date); fnprop(obj(date), "now", g_now);
    JsObj *m = mkns("Math");
    fnprop(m, "floor", m_floor); fnprop(m, "ceil", m_ceil); fnprop(m, "round", m_round); fnprop(m, "abs", m_abs);
    fnprop(m, "sqrt", m_sqrt); fnprop(m, "sin", m_sin); fnprop(m, "cos", m_cos); fnprop(m, "tan", m_tan); fnprop(m, "log", m_log);
    fnprop(m, "exp", m_exp); fnprop(m, "trunc", m_trunc); fnprop(m, "sign", m_sign); fnprop(m, "pow", m_pow);
    fnprop(m, "random", m_random); fnprop(m, "min", m_min); fnprop(m, "max", m_max);
    setprop(m, cstr("PI"), js_num(3.141592653589793)); setprop(m, cstr("E"), js_num(2.718281828459045));
    JsObj *j = mkns("JSON"); fnprop(j, "stringify", g_stringify); fnprop(j, "parse", g_jsonparse);
    ctl = C_NONE;
}

int js_run(const c16 *src, int len) {
    failed = 0; ctl = C_NONE; errmsg[0] = 0; steps = 0;
    if (!lex(src, len)) return 0;
    tp = 0; parsing = 1;
    Node *head = 0, **tail = &head;
    while (!failed && cur()->t != T_EOF) { *tail = statement(); if (*tail) tail = &(*tail)->next; }
    parsing = 0;
    if (failed) return 0;
    hoist(head, global);
    exec_list(head, global);
    if (ctl == C_THROW) {
        JsStr *m = js_tostr(ctlv); int i = 0;
        for (; i < m->len && i < 190; i++) errmsg[i] = m->s[i];
        errmsg[i] = 0; ctl = C_NONE; return 0;
    }
    ctl = C_NONE;
    if (oom) { const char *m = "メモリが足りません"; int i = 0; for (; m[i]; i++) errmsg[i] = (u8)m[i]; errmsg[i] = 0; return 0; }
    return 1;
}
JsVal js_make_array(JsVal *items, int n) { JsObj *a = newarr(); for (int i = 0; i < n; i++) arr_push(a, items[i]); return O(a); }
JsVal js_global_get(const char *name) { Prop *p = find(globalObj->props, cstr(name)); if (p) return p->v; p = find(global->vars, cstr(name)); return p ? p->v : js_undef(); }
int js_call(JsVal fn, JsVal self, JsVal *args, int n) {
    ctl = C_NONE; steps = 0; errmsg[0] = 0;
    call(fn, self, args, n);
    if (ctl == C_THROW) {
        JsStr *m = js_tostr(ctlv); int i = 0;
        for (; i < m->len && i < 190; i++) errmsg[i] = m->s[i];
        errmsg[i] = 0; ctl = C_NONE; return 0;
    }
    ctl = C_NONE;
    return 1;
}
const c16 *js_error(void) { return errmsg; }
