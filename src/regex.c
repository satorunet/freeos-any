/* 正規表現 (JavaScript の書き方)。後戻りしながら照合する。
 * 対応: . [...] [^...] \d\w\s\D\W\S \b\B ^ $ (...) (?:...) (?<名前>...) (?=...) (?!...) | * + ? {n,m} 最短一致 \1 \uXXXX
 * フラグ: g i m s y (u は受け付けるだけ)。非対応: 後読み (?<=...) */
#include "regex.h"

enum { R_CHAR, R_ANY, R_CLASS, R_BOL, R_EOL, R_WORDB, R_NWORDB, R_GROUP, R_ALT, R_REP, R_BACKREF, R_LOOK, R_EMPTY };
typedef struct RNode RNode;
struct RNode {
    u8 t, neg, greedy; int min, max, idx;
    c16 c;
    struct { c16 lo, hi; } *ranges; int nr; u8 cls; /* cls: \d=1 \w=2 \s=4 (否定は +8) のまとまり */
    RNode *child, *alt, *next;   /* alt: 選択肢の続き。next: 並びの次 */
};
struct Regex { RNode *root; int ncap; int icase, multiline, dotall; c16 names[16][24]; int nameIdx[16], nnames; };

static void *(*ra)(u64);
static const c16 *P; static int PN, pi, perr;
static RNode *rn(u8 t) { RNode *n = ra(sizeof(RNode)); memset(n, 0, sizeof *n); n->t = t; return n; }
static Regex *cur;

static int isword(c16 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static int isdigit_(c16 c) { return c >= '0' && c <= '9'; }
static int isspace_(c16 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == 0xA0 || c == 0x3000 || c == 0xFEFF || c == 0x2028 || c == 0x2029; }
static c16 lowerc(c16 c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int hexv(c16 c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

/* エスケープ 1 文字 (クラスの外と中で共通の部分)。cls に \d などを返す */
static c16 escape(int *cls) {
    *cls = 0;
    if (pi >= PN) { perr = 1; return 0; }
    c16 c = P[pi++];
    switch (c) {
    case 'd': *cls = 1; return 0; case 'D': *cls = 9; return 0;
    case 'w': *cls = 2; return 0; case 'W': *cls = 10; return 0;
    case 's': *cls = 4; return 0; case 'S': *cls = 12; return 0;
    case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r'; case 'f': return '\f'; case 'v': return '\v'; case '0': return 0;
    case 'x': { int a = pi + 1 < PN ? hexv(P[pi]) : -1, b = pi + 1 < PN ? hexv(P[pi + 1]) : -1; if (a < 0 || b < 0) return 'x'; pi += 2; return (c16)(a * 16 + b); }
    case 'u': {
        if (pi < PN && P[pi] == '{') { u32 v = 0; pi++; while (pi < PN && P[pi] != '}') v = v * 16 + (u32)hexv(P[pi++]); pi++; return v > 0xFFFF ? 0xFFFD : (c16)v; }
        u32 v = 0; for (int i = 0; i < 4; i++) { int h = pi < PN ? hexv(P[pi]) : -1; if (h < 0) return 'u'; v = v * 16 + h; pi++; }
        return (c16)v;
    }
    case 'c': if (pi < PN) return (c16)(P[pi++] % 32); return 'c';
    }
    return c;
}
static int class_has(const RNode *n, c16 c, int icase) {
    int in = 0;
    if ((n->cls & 1) && isdigit_(c)) in = 1;
    if ((n->cls & 2) && isword(c)) in = 1;
    if ((n->cls & 4) && isspace_(c)) in = 1;
    if ((n->cls & 16) && !isdigit_(c)) in = 1;
    if ((n->cls & 32) && !isword(c)) in = 1;
    if ((n->cls & 64) && !isspace_(c)) in = 1;
    for (int i = 0; i < n->nr && !in; i++) {
        if (c >= n->ranges[i].lo && c <= n->ranges[i].hi) in = 1;
        else if (icase) { c16 l = lowerc(c), u = c >= 'a' && c <= 'z' ? c - 32 : c; if ((l >= n->ranges[i].lo && l <= n->ranges[i].hi) || (u >= n->ranges[i].lo && u <= n->ranges[i].hi)) in = 1; }
    }
    return in != n->neg;
}
static void add_cls(RNode *n, int cls) { if (cls & 8) n->cls |= (u8)((cls & 7) << 4); else n->cls |= (u8)cls; }
static RNode *parse_class(void) { /* [ の次から */
    RNode *n = rn(R_CLASS);
    int cap = 16; n->ranges = ra(sizeof(*n->ranges) * cap);
    if (pi < PN && P[pi] == '^') n->neg = 1, pi++;
    int first = 1;
    while (pi < PN && (P[pi] != ']' || (first && 0))) {
        first = 0;
        c16 lo; int cls = 0;
        if (P[pi] == '\\') { pi++; if (pi < PN && P[pi] == 'b') { pi++; lo = 8; } else lo = escape(&cls); } else lo = P[pi++];
        if (cls) { add_cls(n, cls); continue; }
        c16 hi = lo;
        if (pi + 1 < PN && P[pi] == '-' && P[pi + 1] != ']') {
            pi++;
            if (P[pi] == '\\') { pi++; hi = escape(&cls); if (cls) { add_cls(n, cls); hi = lo; } } else hi = P[pi++];
        }
        if (n->nr == cap) { void *m = ra(sizeof(*n->ranges) * cap * 2); memcpy(m, n->ranges, sizeof(*n->ranges) * cap); n->ranges = m; cap *= 2; }
        n->ranges[n->nr].lo = lo; n->ranges[n->nr].hi = hi; n->nr++;
    }
    if (pi >= PN) perr = 1; else pi++;
    return n;
}
static RNode *parse_alt(void);
static RNode *parse_atom(void) {
    c16 c = P[pi];
    if (c == '(') {
        pi++;
        if (pi + 1 < PN && P[pi] == '?') {
            if (P[pi + 1] == ':') { pi += 2; RNode *g = rn(R_GROUP); g->idx = -1; g->child = parse_alt(); if (pi < PN && P[pi] == ')') pi++; else perr = 1; return g; }
            if (P[pi + 1] == '=' || P[pi + 1] == '!') { RNode *l = rn(R_LOOK); l->neg = P[pi + 1] == '!'; pi += 2; l->child = parse_alt(); if (pi < PN && P[pi] == ')') pi++; else perr = 1; return l; }
            if (P[pi + 1] == '<' && pi + 2 < PN && P[pi + 2] != '=' && P[pi + 2] != '!') { /* 名前付き */
                pi += 2; int k = 0; c16 name[24];
                while (pi < PN && P[pi] != '>' && k < 23) name[k++] = P[pi++];
                name[k] = 0; pi++;
                RNode *g = rn(R_GROUP); g->idx = ++cur->ncap;
                if (cur->nnames < 16) { for (int i = 0; i <= k; i++) cur->names[cur->nnames][i] = name[i]; cur->nameIdx[cur->nnames++] = g->idx; }
                g->child = parse_alt(); if (pi < PN && P[pi] == ')') pi++; else perr = 1; return g;
            }
            perr = 1; return rn(R_EMPTY); /* 後読みなどは非対応 */
        }
        RNode *g = rn(R_GROUP); g->idx = ++cur->ncap;
        g->child = parse_alt();
        if (pi < PN && P[pi] == ')') pi++; else perr = 1;
        return g;
    }
    pi++;
    if (c == '.') return rn(R_ANY);
    if (c == '^') return rn(R_BOL);
    if (c == '$') return rn(R_EOL);
    if (c == '[') return parse_class();
    if (c == '\\') {
        if (pi < PN && P[pi] == 'b') { pi++; return rn(R_WORDB); }
        if (pi < PN && P[pi] == 'B') { pi++; return rn(R_NWORDB); }
        if (pi < PN && P[pi] >= '1' && P[pi] <= '9') { RNode *b = rn(R_BACKREF); while (pi < PN && isdigit_(P[pi])) b->idx = b->idx * 10 + P[pi++] - '0'; return b; }
        if (pi + 1 < PN && P[pi] == 'k' && P[pi + 1] == '<') { /* \k<名前> */
            pi += 2; c16 name[24]; int k = 0; while (pi < PN && P[pi] != '>' && k < 23) name[k++] = P[pi++]; name[k] = 0; pi++;
            RNode *b = rn(R_BACKREF);
            for (int i = 0; i < cur->nnames; i++) { int j = 0; while (name[j] && name[j] == cur->names[i][j]) j++; if (!name[j] && !cur->names[i][j]) b->idx = cur->nameIdx[i]; }
            return b;
        }
        int cls; c16 e = escape(&cls);
        if (cls) { RNode *n = rn(R_CLASS); add_cls(n, cls); return n; }
        RNode *n = rn(R_CHAR); n->c = e; return n;
    }
    RNode *n = rn(R_CHAR); n->c = c; return n;
}
static int parse_int(int *v) { int any = 0; *v = 0; while (pi < PN && isdigit_(P[pi])) *v = *v * 10 + P[pi++] - '0', any = 1; return any; }
static RNode *parse_seq(void) {
    RNode *head = 0, **tail = &head;
    while (pi < PN && P[pi] != '|' && P[pi] != ')' && !perr) {
        RNode *a = parse_atom();
        if (pi < PN) { /* 繰り返し */
            int mn = -1, mx = -1; c16 q = P[pi];
            if (q == '*') mn = 0, mx = -1, pi++;
            else if (q == '+') mn = 1, mx = -1, pi++;
            else if (q == '?') mn = 0, mx = 1, pi++;
            else if (q == '{') {
                int save = pi; pi++; int a1, a2;
                if (parse_int(&a1)) {
                    mn = a1; mx = a1;
                    if (pi < PN && P[pi] == ',') { pi++; mx = parse_int(&a2) ? a2 : -1; }
                    if (pi < PN && P[pi] == '}') pi++; else { pi = save; mn = -1; }
                } else { pi = save; mn = -1; }
                if (mn < 0) { /* ただの { */ }
            }
            if (mn >= 0) {
                RNode *r = rn(R_REP); r->min = mn; r->max = mx; r->child = a; r->greedy = 1;
                if (pi < PN && P[pi] == '?') r->greedy = 0, pi++;
                a = r;
            }
        }
        *tail = a; tail = &a->next;
    }
    return head ? head : rn(R_EMPTY);
}
static RNode *parse_alt(void) {
    RNode *first = parse_seq();
    if (pi < PN && P[pi] == '|') {
        RNode *a = rn(R_ALT); a->child = first; RNode **tail = &a->alt;
        while (pi < PN && P[pi] == '|') { pi++; RNode *o = rn(R_ALT); o->child = parse_seq(); *tail = o; tail = &o->alt; }
        return a;
    }
    return first;
}
Regex *re_compile(const c16 *pat, int n, const char *flags, void *(*alloc)(u64)) {
    ra = alloc; P = pat; PN = n; pi = 0; perr = 0;
    Regex *r = alloc(sizeof(Regex)); memset(r, 0, sizeof *r); cur = r;
    for (const char *f = flags; f && *f; f++) { if (*f == 'i') r->icase = 1; if (*f == 'm') r->multiline = 1; if (*f == 's') r->dotall = 1; }
    r->root = parse_alt();
    if (pi < PN) perr = 1; /* 余った ) など */
    return perr ? 0 : r;
}
int re_ncap(const Regex *r) { return r->ncap; }
int re_group_name(const Regex *r, int i, const c16 **name, int *idx) { if (i >= r->nnames) return 0; *name = r->names[i]; *idx = r->nameIdx[i]; return 1; }

/* ---- 照合 (続きを持ち回る) ---- */
typedef struct Cont Cont;
struct Cont { RNode *n; int kind, count, start; RNode *rep; Cont *next; }; /* kind 0: 並びの続き 1: グループの終わり 2: 繰り返しの続き 3: 先読みの終わり */
static const c16 *S; static int SN; static int *caps; static Regex *R; static long budget; static int depth;
static int m(RNode *n, int i, Cont *k);
static int next(int i, Cont *k) {
    if (!k) return i;
    switch (k->kind) {
    case 0: return m(k->n, i, k->next);
    case 1: { int idx = k->count, old = caps[idx * 2 + 1], olds = caps[idx * 2]; caps[idx * 2] = k->start; caps[idx * 2 + 1] = i; int r = next(i, k->next); if (r < 0) { caps[idx * 2] = olds; caps[idx * 2 + 1] = old; } return r; }
    case 2: { /* 繰り返し: もう 1 回か、ここで終わるか */
        RNode *rep = k->rep; int cnt = k->count;
        if (i == k->start && cnt > rep->min) return -1; /* 空の繰り返しは打ち切る */
        Cont more = {0, 2, cnt + 1, i, rep, k->next};
        int canMore = rep->max < 0 || cnt < rep->max;
        if (cnt < rep->min) return m(rep->child, i, &more);
        if (rep->greedy) { if (canMore) { int r = m(rep->child, i, &more); if (r >= 0) return r; } return next(i, k->next); }
        int r = next(i, k->next); if (r >= 0) return r;
        return canMore ? m(rep->child, i, &more) : -1;
    }
    case 3: return i; /* 先読みの中身が合った */
    }
    return -1;
}
static int m(RNode *n, int i, Cont *k) {
    if (--budget < 0 || depth > 4000) return -1;
    if (!n) return next(i, k);
    depth++;
    int r = -1;
    Cont seq = {n->next, 0, 0, 0, 0, k};
    Cont *after = n->next ? &seq : k;
    switch (n->t) {
    case R_EMPTY: r = next(i, after); break;
    case R_CHAR: if (i < SN && (S[i] == n->c || (R->icase && lowerc(S[i]) == lowerc(n->c)))) r = next(i + 1, after); break;
    case R_ANY: if (i < SN && (R->dotall || (S[i] != '\n' && S[i] != '\r' && S[i] != 0x2028 && S[i] != 0x2029))) r = next(i + 1, after); break;
    case R_CLASS: if (i < SN && class_has(n, S[i], R->icase)) r = next(i + 1, after); break;
    case R_BOL: if (i == 0 || (R->multiline && (S[i - 1] == '\n' || S[i - 1] == '\r'))) r = next(i, after); break;
    case R_EOL: if (i == SN || (R->multiline && (S[i] == '\n' || S[i] == '\r'))) r = next(i, after); break;
    case R_WORDB: case R_NWORDB: { int a = i > 0 && isword(S[i - 1]), b = i < SN && isword(S[i]); if ((a != b) == (n->t == R_WORDB)) r = next(i, after); break; }
    case R_GROUP:
        if (n->idx < 0) r = m(n->child, i, after);
        else { Cont g = {0, 1, n->idx, i, 0, after}; r = m(n->child, i, &g); }
        break;
    case R_ALT: for (RNode *o = n; o && r < 0; o = o->alt) r = m(o->child, i, after); break;
    case R_REP: { Cont c0 = {0, 2, 0, -1, n, after}; r = next(i, &c0); break; }
    case R_BACKREF: {
        int a = n->idx <= R->ncap ? caps[n->idx * 2] : -1, b = n->idx <= R->ncap ? caps[n->idx * 2 + 1] : -1;
        if (a < 0 || b < 0) { r = next(i, after); break; }
        int len = b - a; if (i + len > SN) break;
        int ok = 1; for (int j = 0; j < len && ok; j++) ok = S[a + j] == S[i + j] || (R->icase && lowerc(S[a + j]) == lowerc(S[i + j]));
        if (ok) r = next(i + len, after);
        break;
    }
    case R_LOOK: { Cont end = {0, 3, 0, 0, 0, 0}; int got = m(n->child, i, &end) >= 0; if (got != n->neg) r = next(i, after); break; }
    }
    depth--;
    return r;
}
/* start から探す (sticky なら start でだけ)。見つかれば 1。caps[0..1] が全体、[2k..2k+1] が k 番目のグループ (なければ -1) */
int re_exec(Regex *re, const c16 *s, int len, int start, int sticky, int *capsOut) {
    R = re; S = s; SN = len; caps = capsOut; budget = 2000000;
    for (int st = start; st <= len; st++) {
        for (int i = 0; i < (re->ncap + 1) * 2; i++) caps[i] = -1;
        depth = 0;
        int e = m(re->root, st, 0);
        if (e >= 0) { caps[0] = st; caps[1] = e; return 1; }
        if (sticky || budget < 0) break;
    }
    return 0;
}
