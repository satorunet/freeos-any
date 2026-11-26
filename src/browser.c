/* ブラウザ: HTML を要素の木 (DOM) にして表示し、小さな JavaScript も動かす。
 * 対応: 見出し・強調・リスト・表・フォーム・リンク、DOM の書き換え・イベント・タイマー・alert。
 * 非対応: 画像・CSS の配置・Shift_JIS など。HOME の .html は file:名前.html で開ける。 */
#include "net.h"
#include "js.h"

#define SAVEMAX (16 * 1024 * 1024)
static u8 *raw;
static int clen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void ccopy(char *d, const char *s, int cap) { int n = 0; while (s[n] && n < cap - 1) { d[n] = s[n]; n++; } d[n] = 0; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int prefix(const char *s, const char *p) { while (*p) if (lower(*s++) != *p++) return 0; return 1; }
static int cmpi(const char *a, const char *b) { while (*a && lower(*a) == lower(*b)) a++, b++; return lower(*a) - lower(*b); }
static int tag_is(const char *tag, const char *list) { /* list は " p div br " の形 */
    int n = clen(tag);
    for (const char *p = list; *p; p++) if (*p == ' ') { int i = 0; while (i < n && p[1 + i] == tag[i]) i++; if (i == n && p[1 + n] == ' ') return 1; }
    return 0;
}
static char current[URLLEN];
static c16 status[160];

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

/* ==================== DOM (ページの要素の木) ==================== */
typedef struct Attr { char name[16]; c16 *v; int n; struct Attr *next; } Attr;
enum { D_ELEM, D_TEXT, D_DOC = 9, D_STYLE = 8, D_CLASS = 7 };
typedef struct DNode {
    u8 type, hidden, checked, hasColor;
    char tag[12];
    c16 *text; int tlen, tcap;   /* 文字の節: 中身 / 入力欄: 値 */
    Attr *attrs;
    struct DNode *parent, *first, *last, *next, *prev;
    JsObj *js;
    JsVal onclick, onchange, oninput;
    JsVal listen[6]; char lname[6][12]; int nlisten;
    u32 color;
} DNode;

static u8 *dheap; static u64 dused;
#define DHEAP (24 * 1024 * 1024)
#define JSHEAP (64 * 1024 * 1024)
static void *jsheap;
static void *dalloc(u64 n) {
    n = (n + 7) & ~7ull;
    if (dused + n > DHEAP) return 0;
    void *p = dheap + dused; dused += n; memset(p, 0, n); return p;
}
static DNode *root, *body, *docNode, *cursorParent; /* cursorParent: 読み込み中に足していく場所 */
static c16 pageTitle[80];

static int domFull; /* 要素の領域を使い切った: これ以上は足さない */
static DNode *dnew(u8 type, const char *tag) {
    DNode *n = dalloc(sizeof(DNode));
    if (!n) { domFull = 1; return 0; }
    n->type = type;
    if (tag) ccopy(n->tag, tag, 12);
    return n;
}
static void dappend(DNode *p, DNode *c) {
    if (!p || !c || p == c) return;
    for (DNode *a = p; a; a = a->parent) if (a == c) return; /* 自分の子孫の下には入れない (輪になる) */
    c->parent = p; c->next = 0; c->prev = p->last;
    if (p->last) p->last->next = c; else p->first = c;
    p->last = c;
}
static void dremove(DNode *c) {
    if (!c || !c->parent) return;
    DNode *p = c->parent;
    if (c->prev) c->prev->next = c->next; else p->first = c->next;
    if (c->next) c->next->prev = c->prev; else p->last = c->prev;
    c->parent = c->next = c->prev = 0;
}
static void dinsert_before(DNode *p, DNode *c, DNode *ref) {
    if (!ref || ref->parent != p) { dappend(p, c); return; }
    c->parent = p; c->next = ref; c->prev = ref->prev;
    if (ref->prev) ref->prev->next = c; else p->first = c;
    ref->prev = c;
}
static void text_set(DNode *n, const c16 *s, int len) {
    if (!n) return;
    if (len + 1 > n->tcap) { int cap = len + 32; c16 *t = dalloc((u64)cap * 2); if (!t) return; n->text = t; n->tcap = cap; }
    for (int i = 0; i < len; i++) n->text[i] = s[i];
    n->tlen = len; n->text[len] = 0;
}
static void text_add(DNode *n, c16 c) {
    if (!n) return;
    if (n->tlen + 2 > n->tcap) { int cap = n->tcap ? n->tcap * 2 : 32; c16 *t = dalloc((u64)cap * 2); if (!t) return; if (n->tlen) mcopy(t, n->text, (u64)n->tlen * 2); n->text = t; n->tcap = cap; }
    n->text[n->tlen++] = c; n->text[n->tlen] = 0;
}
static Attr *attr_find(DNode *n, const char *name) { for (Attr *a = n->attrs; a; a = a->next) if (!cmpi(a->name, name)) return a; return 0; }
static void attr_set(DNode *n, const char *name, const c16 *v, int len) {
    if (!n) return;
    Attr *a = attr_find(n, name);
    if (!a) { a = dalloc(sizeof(Attr)); if (!a) return; ccopy(a->name, name, 16); for (char *p = a->name; *p; p++) *p = lower(*p); a->next = n->attrs; n->attrs = a; }
    a->v = dalloc((u64)(len + 1) * 2); if (!a->v) return;
    for (int i = 0; i < len; i++) a->v[i] = v[i];
    a->n = len;
}
/* 属性を ASCII で取り出す (URL など) */
static int attr_c(DNode *n, const char *name, char *out, int cap) {
    Attr *a = attr_find(n, name);
    if (!a) return 0;
    int k = 0;
    for (int i = 0; i < a->n && k < cap - 4; i++) {
        c16 c = a->v[i];
        if (c < 0x80) { if (c >= 32) out[k++] = (char)c; }
        else if (c < 0x800) { out[k++] = 0xC0 | c >> 6; out[k++] = 0x80 | (c & 63); }
        else { out[k++] = 0xE0 | c >> 12; out[k++] = 0x80 | ((c >> 6) & 63); out[k++] = 0x80 | (c & 63); }
    }
    out[k] = 0;
    return 1;
}
static int attr_is(DNode *n, const char *name, const char *val) {
    Attr *a = attr_find(n, name); if (!a) return 0;
    int i = 0; for (; val[i]; i++) if (i >= a->n || lower((char)a->v[i]) != val[i]) return 0;
    return i == a->n;
}
static int is_tag(DNode *n, const char *t) { return n && n->type == D_ELEM && !cmpi(n->tag, t); }
static int text_of(DNode *n, c16 *out, int cap, int k) { /* 子孫の文字をつなげる */
    if (!n) return k;
    if (n->type == D_TEXT) { for (int i = 0; i < n->tlen && k < cap - 1; i++) out[k++] = n->text[i]; out[k] = 0; return k; }
    if (is_tag(n, "script") || is_tag(n, "style")) return k;
    for (DNode *c = n->first; c; c = c->next) k = text_of(c, out, cap, k);
    out[k] = 0;
    return k;
}

/* ---- HTML の読み込み ---- */
static int void_tag(const char *t) { return tag_is(t, " br img input hr meta link area base col embed source track wbr param "); }
static int raw_tag(const char *t) { return tag_is(t, " script style textarea title xmp "); }
static void run_script(DNode *s);
static int parseDepth;
static void parse_html(const u8 *s, u64 n, DNode *into) {
    if (parseDepth > 8) return;
    parseDepth++;
    DNode *saved = cursorParent;
    cursorParent = into;
    for (u64 i = 0; i < n;) {
        if (s[i] == '<' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '!' || s[i + 1] == '?' || (lower(s[i + 1]) >= 'a' && lower(s[i + 1]) <= 'z'))) {
            if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
                for (i += 4; i + 2 < n && !(s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>'); i++) {}
                i += 3; continue;
            }
            u64 st = ++i; u8 q = 0;
            for (; i < n; i++) {
                if (q) { if (s[i] == q) q = 0; }
                else if ((s[i] == '"' || s[i] == '\'') && i > st && (s[i - 1] == '=' || s[i - 1] == ' ')) q = s[i];
                else if (s[i] == '>') break;
            }
            int tn = (int)(i - st); i++;
            if (s[st] == '!' || s[st] == '?') continue;
            int close = tn && s[st] == '/';
            char tag[12]; int m = 0;
            for (int k = close; k < tn && m < 11 && s[st + k] > 32 && s[st + k] != '/' && s[st + k] != '>'; k++) tag[m++] = lower(s[st + k]);
            tag[m] = 0;
            if (!m) continue;
            if (close) {
                for (DNode *p = cursorParent; p && p != into->parent; p = p->parent)
                    if (p->type == D_ELEM && !cmpi(p->tag, tag)) { cursorParent = p->parent ? p->parent : into; break; }
                if (!cmpi(tag, "p") && !is_tag(cursorParent, "p")) {} /* 余った </p> は無視 */
                continue;
            }
            /* 閉じ忘れを補う: <p> <li> <tr> <td> <option> */
            if (tag_is(tag, " p div ul ol table h1 h2 h3 h4 h5 h6 pre blockquote form section article header footer nav ") && is_tag(cursorParent, "p")) cursorParent = cursorParent->parent;
            if (tag_is(tag, " li ")) for (DNode *p = cursorParent; p && !is_tag(p, "ul") && !is_tag(p, "ol"); p = p->parent) if (is_tag(p, "li")) { cursorParent = p->parent; break; }
            if (tag_is(tag, " tr ")) for (DNode *p = cursorParent; p && !is_tag(p, "table"); p = p->parent) if (is_tag(p, "tr")) { cursorParent = p->parent; break; }
            if (tag_is(tag, " td th ")) for (DNode *p = cursorParent; p && !is_tag(p, "tr"); p = p->parent) if (is_tag(p, "td") || is_tag(p, "th")) { cursorParent = p->parent; break; }
            if (tag_is(tag, " option ") && is_tag(cursorParent, "option")) cursorParent = cursorParent->parent;
            if (tag_is(tag, " dt dd ")) for (DNode *p = cursorParent; p && !is_tag(p, "dl"); p = p->parent) if (is_tag(p, "dt") || is_tag(p, "dd")) { cursorParent = p->parent; break; }
            DNode *e = dnew(D_ELEM, tag);
            if (!e) break; /* 要素の領域がいっぱい: ここまでで表示する */
            /* 属性 */
            const u8 *t = s + st; int k = m;
            while (k < tn) {
                while (k < tn && (t[k] <= 32 || t[k] == '/')) k++;
                int ns = k; while (k < tn && t[k] > 32 && t[k] != '=' && t[k] != '/' && t[k] != '>') k++;
                char an[16]; int al = 0; for (int j = ns; j < k && al < 15; j++) an[al++] = lower(t[j]); an[al] = 0;
                while (k < tn && t[k] <= 32) k++;
                static c16 val[2048]; int vl = 0;
                if (k < tn && t[k] == '=') {
                    k++; while (k < tn && t[k] <= 32) k++;
                    u64 vs, ve;
                    if (k < tn && (t[k] == '"' || t[k] == '\'')) { u8 qq = t[k++]; vs = k; while (k < tn && t[k] != qq) k++; ve = k; k++; }
                    else { vs = k; while (k < tn && t[k] > 32 && t[k] != '>') k++; ve = k; }
                    for (u64 p = vs; p < ve && vl < 2047;) {
                        u32 c;
                        if (t[p] == '&') { p++; c = entity(t, ve, &p); } else c = next_char(t, ve, &p);
                        val[vl++] = (c16)c;
                    }
                }
                if (al) attr_set(e, an, val, vl);
                if (k == ns) k++;
            }
            dappend(cursorParent, e);
            if (!cmpi(tag, "body")) body = e;
            if (raw_tag(tag)) { /* 中身はそのまま文字 */
                u64 cs = i;
                for (; i + m + 1 < n; i++) if (s[i] == '<' && s[i + 1] == '/' && prefix((const char *)s + i + 2, tag)) break;
                u64 ce = i < n ? i : n;
                DNode *tx = dnew(D_TEXT, 0);
                if (!tx) break;
                for (u64 p = cs; p < ce;) {
                    u32 c = !cmpi(tag, "script") || !cmpi(tag, "style") ? next_char(s, ce, &p) : s[p] == '&' ? (p++, entity(s, ce, &p)) : next_char(s, ce, &p);
                    if (c != '\r') text_add(tx, (c16)c);
                }
                if (!cmpi(tag, "textarea")) text_set(e, tx->text ? tx->text : u"", tx->tlen); else dappend(e, tx);
                while (i < n && s[i] != '>') i++;
                i++;
                if (!cmpi(tag, "title")) { int l = tx->tlen < 79 ? tx->tlen : 79; for (int j = 0; j < l; j++) pageTitle[j] = tx->text[j]; pageTitle[l] = 0; }
                if (!cmpi(tag, "script")) run_script(e);
                continue;
            }
            if (!cmpi(tag, "input")) { Attr *v = attr_find(e, "value"); if (v) text_set(e, v->v, v->n); if (attr_find(e, "checked")) e->checked = 1; }
            if (!void_tag(tag)) cursorParent = e;
            continue;
        }
        /* 文字 */
        DNode *tx = cursorParent->last && cursorParent->last->type == D_TEXT ? cursorParent->last : 0;
        if (!tx) { tx = dnew(D_TEXT, 0); if (!tx) break; dappend(cursorParent, tx); }
        while (i < n && s[i] != '<') {
            u32 c;
            if (s[i] == '&') { i++; c = entity(s, n, &i); } else c = next_char(s, n, &i);
            if (c != '\r') text_add(tx, (c16)c);
        }
    }
    cursorParent = saved ? saved : cursorParent;
    parseDepth--;
}
static DNode *new_document(void) {
    dused = 0; domFull = 0;
    docNode = dnew(D_DOC, "#document");
    root = docNode; body = 0; pageTitle[0] = 0;
    return docNode;
}
static DNode *find_tag(DNode *n, const char *tag) {
    for (DNode *c = n ? n->first : 0; c; c = c->next) {
        if (is_tag(c, tag)) return c;
        DNode *r = find_tag(c, tag); if (r) return r;
    }
    return 0;
}
static DNode *find_id(DNode *n, const c16 *id, int len) {
    for (DNode *c = n ? n->first : 0; c; c = c->next) {
        if (c->type == D_ELEM) {
            Attr *a = attr_find(c, "id");
            if (a && a->n == len) { int i = 0; while (i < len && a->v[i] == id[i]) i++; if (i == len) return c; }
            DNode *r = find_id(c, id, len); if (r) return r;
        }
    }
    return 0;
}
static int has_class(DNode *n, const c16 *cls, int len) {
    Attr *a = attr_find(n, "class"); if (!a) return 0;
    for (int i = 0; i < a->n;) {
        while (i < a->n && a->v[i] == ' ') i++;
        int s = i; while (i < a->n && a->v[i] != ' ') i++;
        if (i - s == len) { int k = 0; while (k < len && a->v[s + k] == cls[k]) k++; if (k == len) return 1; }
    }
    return 0;
}
/* 簡単なセレクター: #id / .class / tag / tag.class / [name=x] は非対応 */
static int match(DNode *n, const c16 *sel, int len) {
    if (n->type != D_ELEM) return 0;
    int dot = -1; for (int i = 0; i < len; i++) if (sel[i] == '.' || sel[i] == '#') { dot = i; break; }
    if (dot != 0) { int tl = dot < 0 ? len : dot; char t[12]; int k = 0; for (int i = 0; i < tl && k < 11; i++) t[k++] = lower((char)sel[i]); t[k] = 0; if (tl && cmpi(n->tag, t)) return 0; }
    if (dot >= 0 && sel[dot] == '.') return has_class(n, sel + dot + 1, len - dot - 1);
    if (dot >= 0 && sel[dot] == '#') { Attr *a = attr_find(n, "id"); int l = len - dot - 1; if (!a || a->n != l) return 0; for (int i = 0; i < l; i++) if (a->v[i] != sel[dot + 1 + i]) return 0; }
    return 1;
}
static int query(DNode *n, const c16 *sel, int len, DNode **out, int max, int k) {
    /* 子孫セレクター "div p" は最後の部分だけ見る */
    int last = 0; for (int i = 0; i < len; i++) if (sel[i] == ' ' || sel[i] == '>') last = i + 1;
    sel += last; len -= last;
    while (len && sel[len - 1] == ' ') len--;
    for (DNode *c = n ? n->first : 0; c && k < max; c = c->next) {
        if (match(c, sel, len)) out[k++] = c;
        k = query(c, sel, len, out, max, k);
    }
    return k;
}

/* ==================== JavaScript とページのつなぎ ==================== */
static int domDirty;       /* 中身が変わった: 並べ直す */
static c16 consoleLine[160], alertText[400];
static int alertOpen;
#define NTIMER 32
static struct { JsVal fn; u64 due, every; int id, used; } timers[NTIMER];
static int nextTimer = 1;
static u8 *fetchBuf;

void *host_jsobj_slot(void *node, JsObj ***slot) { *slot = &((DNode *)node)->js; return 0; }
static DNode *N(JsVal v) { return js_host_node(v); }
static JsVal Hs(const c16 *s, int n) { return js_str(s, n); }
static JsStr *argstr(JsVal *a, int n, int i) { return js_tostr(i < n ? a[i] : js_undef()); }
static void set_children_text(DNode *e, const c16 *s, int len) {
    while (e->first) dremove(e->first);
    DNode *t = dnew(D_TEXT, 0); if (t) { text_set(t, s, len); dappend(e, t); }
    domDirty = 1;
}
static void js_str_utf8(JsStr *s, u8 *out, u64 cap, u64 *len) {
    u64 k = 0;
    for (int i = 0; i < s->len && k + 4 < cap; i++) {
        c16 c = s->s[i];
        if (c < 0x80) out[k++] = (u8)c;
        else if (c < 0x800) { out[k++] = 0xC0 | c >> 6; out[k++] = 0x80 | (c & 63); }
        else { out[k++] = 0xE0 | c >> 12; out[k++] = 0x80 | ((c >> 6) & 63); out[k++] = 0x80 | (c & 63); }
    }
    *len = k;
}
static void set_inner_html(DNode *e, JsStr *html) {
    while (e->first) dremove(e->first);
    static u8 buf[256 * 1024]; u64 len;
    js_str_utf8(html, buf, sizeof buf, &len);
    DNode *save = cursorParent;
    parse_html(buf, len, e);
    cursorParent = save;
    domDirty = 1;
}
/* document のメソッド */
static JsVal nul(void) { JsVal v = {JS_NULL, 0, 0}; return v; }
static JsVal d_getById(JsVal self, JsVal *a, int n) { (void)self; JsStr *id = argstr(a, n, 0); DNode *r = find_id(docNode, id->s, id->len); return r ? js_host(r) : nul(); }
static JsVal d_query(JsVal self, JsVal *a, int n) {
    DNode *base = N(self) ? N(self) : docNode; if (base->type == D_DOC) base = docNode;
    JsStr *s = argstr(a, n, 0); DNode *r[1];
    if (s->len && s->s[0] == '#' ) { DNode *x = find_id(docNode, s->s + 1, s->len - 1); return x ? js_host(x) : nul(); }
    return query(base, s->s, s->len, r, 1, 0) ? js_host(r[0]) : nul();
}
static JsVal list_of(DNode **r, int k) {
    JsVal args[64]; int m = k < 64 ? k : 64;
    for (int i = 0; i < m; i++) args[i] = js_host(r[i]);
    return js_make_array(args, m);
}
static JsVal d_queryAll(JsVal self, JsVal *a, int n) {
    DNode *base = N(self) ? N(self) : docNode; if (base->type == D_DOC) base = docNode;
    JsStr *s = argstr(a, n, 0); static DNode *r[64];
    return list_of(r, query(base, s->s, s->len, r, 64, 0));
}
static JsVal d_byTag(JsVal self, JsVal *a, int n) {
    DNode *base = N(self) ? N(self) : docNode; if (base->type == D_DOC) base = docNode;
    JsStr *s = argstr(a, n, 0); static DNode *r[64];
    return list_of(r, query(base, s->s, s->len, r, 64, 0));
}
static JsVal d_byClass(JsVal self, JsVal *a, int n) {
    DNode *base = N(self) ? N(self) : docNode; if (base->type == D_DOC) base = docNode;
    JsStr *s = argstr(a, n, 0); static DNode *r[64]; static c16 sel[128]; sel[0] = '.';
    int l = s->len < 120 ? s->len : 120; for (int i = 0; i < l; i++) sel[i + 1] = s->s[i];
    return list_of(r, query(base, sel, l + 1, r, 64, 0));
}
static JsVal d_create(JsVal self, JsVal *a, int n) {
    (void)self; JsStr *s = argstr(a, n, 0); char t[12]; int k = 0;
    for (int i = 0; i < s->len && k < 11; i++) t[k++] = lower((char)s->s[i]);
    t[k] = 0;
    DNode *e = dnew(D_ELEM, t);
    return e ? js_host(e) : nul();
}
static JsVal d_createText(JsVal self, JsVal *a, int n) { (void)self; JsStr *s = argstr(a, n, 0); DNode *t = dnew(D_TEXT, 0); if (!t) return nul(); text_set(t, s->s, s->len); return js_host(t); }
static JsVal d_write(JsVal self, JsVal *a, int n) {
    (void)self;
    for (int i = 0; i < n; i++) {
        JsStr *s = js_tostr(a[i]); static u8 buf[128 * 1024]; u64 len;
        js_str_utf8(s, buf, sizeof buf, &len);
        parse_html(buf, len, cursorParent ? cursorParent : (body ? body : docNode));
    }
    domDirty = 1;
    return js_undef();
}
/* 要素のメソッド */
static JsVal e_append(JsVal self, JsVal *a, int n) { DNode *p = N(self), *c = n ? N(a[0]) : 0; if (p && c) { if (p->type == D_DOC) p = body ? body : docNode; dremove(c); dappend(p, c); domDirty = 1; } return n ? a[0] : js_undef(); }
static JsVal e_insertBefore(JsVal self, JsVal *a, int n) { DNode *p = N(self), *c = n ? N(a[0]) : 0, *r = n > 1 ? N(a[1]) : 0; if (p && c) { dremove(c); dinsert_before(p, c, r); domDirty = 1; } return n ? a[0] : js_undef(); }
static JsVal e_removeChild(JsVal self, JsVal *a, int n) { (void)self; DNode *c = n ? N(a[0]) : 0; if (c) { dremove(c); domDirty = 1; } return n ? a[0] : js_undef(); }
static JsVal e_remove(JsVal self, JsVal *a, int n) { (void)a; (void)n; DNode *c = N(self); if (c) { dremove(c); domDirty = 1; } return js_undef(); }
static JsVal e_setAttr(JsVal self, JsVal *a, int n) {
    DNode *e = N(self); JsStr *k = argstr(a, n, 0), *v = argstr(a, n, 1); char name[16]; int l = 0;
    for (int i = 0; i < k->len && l < 15; i++) name[l++] = lower((char)k->s[i]);
    name[l] = 0;
    if (e) { attr_set(e, name, v->s, v->len); if (!cmpi(name, "value")) text_set(e, v->s, v->len); domDirty = 1; }
    return js_undef();
}
static JsVal e_getAttr(JsVal self, JsVal *a, int n) {
    DNode *e = N(self); JsStr *k = argstr(a, n, 0); char name[16]; int l = 0;
    for (int i = 0; i < k->len && l < 15; i++) name[l++] = lower((char)k->s[i]);
    name[l] = 0;
    Attr *at = e ? attr_find(e, name) : 0;
    return at ? Hs(at->v, at->n) : nul();
}
static JsVal e_listen(JsVal self, JsVal *a, int n) {
    DNode *e = N(self); if (!e) e = docNode; /* window.addEventListener も document に */
    JsStr *t = argstr(a, n, 0);
    if (e->nlisten < 6 && n > 1 && js_is_func(a[1])) {
        int k = 0; for (; k < t->len && k < 11; k++) e->lname[e->nlisten][k] = (char)t->s[k];
        e->lname[e->nlisten][k] = 0;
        e->listen[e->nlisten++] = a[1];
    }
    return js_undef();
}
static void fire(DNode *e, const char *type, JsVal ev);
/* insertAdjacentHTML(位置, HTML): beforebegin / afterbegin / beforeend / afterend */
static JsVal e_insertAdjacent(JsVal self, JsVal *a, int n) {
    DNode *e = N(self); if (!e || n < 2) return js_undef();
    JsStr *pos = js_tostr(a[0]);
    DNode *tmp = dnew(D_ELEM, "div");
    if (!tmp) return js_undef();
    if (N(a[1])) { DNode *c = N(a[1]); dremove(c); dappend(tmp, c); }
    else { static u8 buf[128 * 1024]; u64 len; js_str_utf8(js_tostr(a[1]), buf, sizeof buf, &len); DNode *save = cursorParent; parse_html(buf, len, tmp); cursorParent = save; }
    while (tmp->first) {
        DNode *c = tmp->first; dremove(c);
        if (js_key_is(pos, "beforebegin") && e->parent) dinsert_before(e->parent, c, e);
        else if (js_key_is(pos, "afterbegin")) dinsert_before(e, c, e->first);
        else if (js_key_is(pos, "afterend") && e->parent) dinsert_before(e->parent, c, e->next);
        else dappend(e, c);
    }
    domDirty = 1;
    return js_undef();
}
static JsVal e_prepend(JsVal self, JsVal *a, int n) { DNode *p = N(self); for (int i = n - 1; p && i >= 0; i--) { DNode *c = N(a[i]); if (!c) { JsStr *t = js_tostr(a[i]); c = dnew(D_TEXT, 0); if (!c) break; text_set(c, t->s, t->len); } else dremove(c); dinsert_before(p, c, p->first); } domDirty = 1; return js_undef(); }
static JsVal e_click(JsVal self, JsVal *a, int n) { (void)a; (void)n; DNode *e = N(self); if (e) fire(e, "click", js_undef()); return js_undef(); }
static JsVal e_noop(JsVal self, JsVal *a, int n) { (void)self; (void)a; (void)n; return js_undef(); }
/* classList */
static void class_edit(DNode *e, JsStr *c, int mode) { /* 0 追加 1 削除 2 切替 */
    static c16 buf[512]; int k = 0, has = has_class(e, c->s, c->len);
    Attr *at = attr_find(e, "class");
    if (at) for (int i = 0; i < at->n;) {
        while (i < at->n && at->v[i] == ' ') i++;
        int s = i; while (i < at->n && at->v[i] != ' ') i++;
        int same = i - s == c->len; for (int j = 0; same && j < c->len; j++) if (at->v[s + j] != c->s[j]) same = 0;
        if (same && (mode == 1 || mode == 2)) continue;
        if (i > s && k + (i - s) + 1 < 500) { if (k) buf[k++] = ' '; for (int j = s; j < i; j++) buf[k++] = at->v[j]; }
    }
    if ((mode == 0 && !has) || (mode == 2 && !has)) { if (k) buf[k++] = ' '; for (int j = 0; j < c->len && k < 500; j++) buf[k++] = c->s[j]; }
    attr_set(e, "class", buf, k);
    domDirty = 1;
}
static DNode *class_owner(JsVal self) { DNode *p = N(self); return p && p->type == D_CLASS ? p->parent : p; }
static JsVal c_add(JsVal self, JsVal *a, int n) { DNode *e = class_owner(self); for (int i = 0; e && i < n; i++) class_edit(e, js_tostr(a[i]), 0); return js_undef(); }
static JsVal c_remove(JsVal self, JsVal *a, int n) { DNode *e = class_owner(self); for (int i = 0; e && i < n; i++) class_edit(e, js_tostr(a[i]), 1); return js_undef(); }
static JsVal c_toggle(JsVal self, JsVal *a, int n) { DNode *e = class_owner(self); if (e && n) { class_edit(e, js_tostr(a[0]), 2); JsStr *c = js_tostr(a[0]); return js_bool(has_class(e, c->s, c->len)); } return js_bool(0); }
static JsVal c_contains(JsVal self, JsVal *a, int n) { DNode *e = class_owner(self); JsStr *c = argstr(a, n, 0); return js_bool(e && has_class(e, c->s, c->len)); }

static JsVal nodeval(DNode *n) { return n ? js_host(n) : nul(); }
static JsVal children_of(DNode *e, int elementsOnly) {
    static DNode *r[64]; int k = 0;
    for (DNode *c = e->first; c && k < 64; c = c->next) if (!elementsOnly || c->type == D_ELEM) r[k++] = c;
    return list_of(r, k);
}
static u32 parse_color(JsStr *s);
JsVal host_get(void *p, const JsStr *k, int *found) {
    DNode *e = p; *found = 1;
    #define K(s) js_key_is(k, s)
    if (e->type == D_STYLE) {
        DNode *o = e->parent;
        if (K("display")) return js_cstr(o->hidden ? "none" : "");
        if (K("color")) return js_cstr("");
        *found = 0; return js_undef();
    }
    if (e->type == D_CLASS) {
        if (K("add")) return js_native(c_add); if (K("remove")) return js_native(c_remove);
        if (K("toggle")) return js_native(c_toggle); if (K("contains")) return js_native(c_contains);
        *found = 0; return js_undef();
    }
    if (e->type == D_DOC) {
        if (K("getElementById")) return js_native(d_getById);
        if (K("querySelector")) return js_native(d_query);
        if (K("querySelectorAll")) return js_native(d_queryAll);
        if (K("getElementsByTagName")) return js_native(d_byTag);
        if (K("getElementsByClassName")) return js_native(d_byClass);
        if (K("createElement")) return js_native(d_create);
        if (K("createTextNode")) return js_native(d_createText);
        if (K("write") || K("writeln")) return js_native(d_write);
        if (K("addEventListener")) return js_native(e_listen);
        if (K("removeEventListener")) return js_native(e_noop);
        if (K("body")) return nodeval(body ? body : find_tag(docNode, "body"));
        if (K("documentElement")) return nodeval(find_tag(docNode, "html"));
        if (K("head")) return nodeval(find_tag(docNode, "head"));
        if (K("title")) { int l = 0; while (pageTitle[l]) l++; return Hs(pageTitle, l); }
        if (K("readyState")) return js_cstr("complete");
        if (K("cookie")) { static char ck[4096]; cookie_doc_get(current, ck, sizeof ck); return js_cstr(ck); }
        if (K("referrer") || K("domain")) return js_cstr("");
        if (K("hidden")) return js_bool(0);
        if (K("visibilityState")) return js_cstr("visible");
        if (K("URL")) return js_cstr(current);
        *found = 0; return js_undef();
    }
    if (e->type == D_TEXT) {
        if (K("textContent") || K("nodeValue") || K("data")) return Hs(e->text ? e->text : u"", e->tlen);
        if (K("nodeType")) return js_num(3);
        if (K("parentNode")) return nodeval(e->parent);
        *found = 0; return js_undef();
    }
    if (K("textContent") || K("innerText")) { static c16 b[16384]; int l = text_of(e, b, 16384, 0); return Hs(b, l); }
    if (K("innerHTML")) { static c16 b[16384]; int l = text_of(e, b, 16384, 0); return Hs(b, l); } /* 簡易: 文字だけ */
    if (K("value")) return Hs(e->text ? e->text : u"", e->text ? e->tlen : 0);
    if (K("checked")) return js_bool(e->checked);
    if (K("tagName") || K("nodeName")) { c16 t[12]; int i = 0; for (; e->tag[i]; i++) t[i] = e->tag[i] >= 'a' && e->tag[i] <= 'z' ? e->tag[i] - 32 : e->tag[i]; return Hs(t, i); }
    if (K("nodeType")) return js_num(1);
    if (K("id") || K("className") || K("href") || K("src") || K("name") || K("type") || K("title") || K("placeholder") || K("lang") || K("dir") || K("alt")) {
        Attr *a = attr_find(e, K("className") ? "class" : K("id") ? "id" : K("href") ? "href" : K("src") ? "src" : K("name") ? "name" : K("type") ? "type" : K("title") ? "title" : K("lang") ? "lang" : K("dir") ? "dir" : K("alt") ? "alt" : "placeholder");
        if (a && K("href")) { char h[URLLEN], u[URLLEN]; attr_c(e, "href", h, URLLEN); url_join(current, h, u); return js_cstr(u); }
        return a ? Hs(a->v, a->n) : js_cstr("");
    }
    if (K("style")) { DNode *s = dnew(D_STYLE, 0); if (!s) return nul(); s->parent = e; return js_host(s); }
    if (K("classList")) { DNode *s = dnew(D_CLASS, 0); if (!s) return nul(); s->parent = e; return js_host(s); }
    if (K("parentNode") || K("parentElement")) return nodeval(e->parent && e->parent->type == D_ELEM ? e->parent : 0);
    if (K("children")) return children_of(e, 1);
    if (K("childNodes")) return children_of(e, 0);
    if (K("firstChild")) return nodeval(e->first);
    if (K("lastChild")) return nodeval(e->last);
    if (K("nextSibling")) return nodeval(e->next);
    if (K("previousSibling")) return nodeval(e->prev);
    if (K("firstElementChild")) { for (DNode *c = e->first; c; c = c->next) if (c->type == D_ELEM) return js_host(c); return nul(); }
    if (K("hidden")) return js_bool(e->hidden);
    if (K("disabled")) return js_bool(attr_find(e, "disabled") != 0);
    if (K("onclick")) return e->onclick.t ? e->onclick : nul();
    if (K("appendChild") || K("append")) return js_native(e_append);
    if (K("insertBefore")) return js_native(e_insertBefore);
    if (K("removeChild")) return js_native(e_removeChild);
    if (K("remove")) return js_native(e_remove);
    if (K("setAttribute")) return js_native(e_setAttr);
    if (K("getAttribute")) return js_native(e_getAttr);
    if (K("addEventListener")) return js_native(e_listen);
    if (K("removeEventListener") || K("focus") || K("blur") || K("scrollIntoView")) return js_native(e_noop);
    if (K("click")) return js_native(e_click);
    if (K("insertAdjacentHTML") || K("insertAdjacentElement")) return js_native(e_insertAdjacent);
    if (K("prepend")) return js_native(e_prepend);
    if (K("querySelector")) return js_native(d_query);
    if (K("querySelectorAll")) return js_native(d_queryAll);
    if (K("getElementsByTagName")) return js_native(d_byTag);
    if (K("getElementsByClassName")) return js_native(d_byClass);
    *found = 0;
    return js_undef();
    #undef K
}
int host_set(void *p, const JsStr *k, JsVal v) {
    DNode *e = p;
    #define K(s) js_key_is(k, s)
    if (e->type == D_STYLE) {
        DNode *o = e->parent;
        if (K("display")) { JsStr *s = js_tostr(v); o->hidden = js_key_is(s, "none"); domDirty = 1; return 1; }
        if (K("visibility")) { JsStr *s = js_tostr(v); o->hidden = js_key_is(s, "hidden"); domDirty = 1; return 1; }
        if (K("color")) { u32 c = parse_color(js_tostr(v)); o->hasColor = c != 0xFFFFFFFF; o->color = c; domDirty = 1; return 1; }
        return 1; /* ほかのスタイルは無視 */
    }
    if (e->type == D_DOC) {
        if (K("cookie")) { static u8 b[2048]; u64 l; js_str_utf8(js_tostr(v), b, sizeof b - 1, &l); b[l] = 0; cookie_doc_set(current, (const char *)b); return 1; }
        if (K("title")) { JsStr *s = js_tostr(v); int l = s->len < 79 ? s->len : 79; for (int i = 0; i < l; i++) pageTitle[i] = s->s[i]; pageTitle[l] = 0; return 1; }
        return 0;
    }
    if (e->type == D_TEXT) { if (K("textContent") || K("nodeValue") || K("data")) { JsStr *s = js_tostr(v); text_set(e, s->s, s->len); domDirty = 1; return 1; } return 0; }
    if (K("textContent") || K("innerText")) { JsStr *s = js_tostr(v); set_children_text(e, s->s, s->len); return 1; }
    if (K("innerHTML")) { set_inner_html(e, js_tostr(v)); return 1; }
    if (K("value")) { JsStr *s = js_tostr(v); text_set(e, s->s, s->len); domDirty = 1; return 1; }
    if (K("checked")) { e->checked = js_truthy(v); domDirty = 1; return 1; }
    if (K("hidden")) { e->hidden = js_truthy(v); domDirty = 1; return 1; }
    if (K("disabled")) { if (js_truthy(v)) attr_set(e, "disabled", u"", 0); else { for (Attr **a = &e->attrs; *a; a = &(*a)->next) if (!cmpi((*a)->name, "disabled")) { *a = (*a)->next; break; } } domDirty = 1; return 1; }
    if (K("id") || K("className") || K("href") || K("src") || K("name") || K("type") || K("title") || K("placeholder") || K("lang") || K("dir") || K("alt")) {
        JsStr *s = js_tostr(v);
        attr_set(e, K("className") ? "class" : K("id") ? "id" : K("href") ? "href" : K("src") ? "src" : K("name") ? "name" : K("type") ? "type" : K("title") ? "title" : K("lang") ? "lang" : K("dir") ? "dir" : K("alt") ? "alt" : "placeholder", s->s, s->len);
        domDirty = 1; return 1;
    }
    if (K("onclick")) { e->onclick = v; return 1; }
    if (K("onchange")) { e->onchange = v; return 1; }
    if (K("oninput")) { e->oninput = v; return 1; }
    return 0;
    #undef K
}

/* ---- 色 (style の color) ---- */
static u32 parse_color(JsStr *s) {
    static const struct { const char *n; u32 c; } names[] = {{"red", 0xE5484D}, {"green", 0x1E8E3E}, {"blue", 0x2455C8}, {"black", 0x111111}, {"white", 0xFFFFFF},
        {"gray", 0x808080}, {"grey", 0x808080}, {"orange", 0xF97316}, {"purple", 0x7C3AED}, {"navy", 0x1E2A78}, {"teal", 0x0D9488}, {"maroon", 0x800000}, {"brown", 0x8B4513}, {"pink", 0xEC4899}};
    int i = 0; while (i < s->len && s->s[i] == ' ') i++;
    if (i < s->len && s->s[i] == '#') {
        u32 v = 0; int d = 0;
        for (i++; i < s->len && d < 6; i++, d++) { c16 c = s->s[i] | 32; int x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; if (x < 0) break; v = v << 4 | x; }
        if (d == 3) v = ((v >> 8) & 15) * 0x110000 | ((v >> 4) & 15) * 0x1100 | (v & 15) * 0x11;
        return d == 3 || d == 6 ? v : 0xFFFFFFFF;
    }
    if (s->len - i > 4 && (s->s[i] | 32) == 'r' && (s->s[i + 1] | 32) == 'g' && (s->s[i + 2] | 32) == 'b') {
        u32 v[3] = {0}; int k = 0;
        for (; i < s->len && k < 3; i++) { if (s->s[i] >= '0' && s->s[i] <= '9') { while (i < s->len && s->s[i] >= '0' && s->s[i] <= '9') v[k] = v[k] * 10 + s->s[i++] - '0'; k++; } }
        return (v[0] & 255) << 16 | (v[1] & 255) << 8 | (v[2] & 255);
    }
    for (unsigned j = 0; j < sizeof names / sizeof *names; j++) if (js_key_is(s, names[j].n)) return names[j].c;
    return 0xFFFFFFFF;
}
static void style_attr(DNode *e) { /* style="color: …; display: none" */
    Attr *a = attr_find(e, "style"); if (!a) return;
    for (int i = 0; i < a->n;) {
        int ks = i; while (i < a->n && a->v[i] != ':' && a->v[i] != ';') i++;
        int ke = i; if (i < a->n && a->v[i] == ':') i++;
        int vs = i; while (i < a->n && a->v[i] != ';') i++;
        int ve = i; i++;
        while (ks < ke && a->v[ks] == ' ') ks++;
        while (ke > ks && a->v[ke - 1] == ' ') ke--;
        JsStr *key = (JsStr *)dalloc(sizeof(JsStr) + (u64)(ke - ks + 1) * 2), *val = (JsStr *)dalloc(sizeof(JsStr) + (u64)(ve - vs + 1) * 2);
        if (!key || !val) return;
        key->len = ke - ks; for (int j = 0; j < key->len; j++) key->s[j] = a->v[ks + j] | (a->v[ks + j] < 128 ? 32 : 0);
        while (vs < ve && a->v[vs] == ' ') vs++;
        val->len = ve - vs; for (int j = 0; j < val->len; j++) val->s[j] = a->v[vs + j];
        if (js_key_is(key, "display") && val->len >= 4 && val->s[0] == 'n' && val->s[1] == 'o') e->hidden = 1;
        if (js_key_is(key, "color")) { u32 c = parse_color(val); if (c != 0xFFFFFFFF) e->color = c, e->hasColor = 1; }
    }
}

/* ---- スクリプトの実行 ---- */
static void js_report(void) {
    const c16 *m = js_error(); if (!m[0]) return;
    scopy(status, u"JS エラー: ", 160); scat(status, m, 160);
}
static c16 *srcbuf;
#define SRCMAX (512 * 1024)
static int load_bytes(const char *url, u8 *buf, u64 cap, u64 *len);
static void run_text(const u8 *s, u64 n) {
    int k = 0;
    for (u64 i = 0; i < n && k < SRCMAX - 1;) srcbuf[k++] = (c16)next_char(s, n, &i);
    if (!js_run(srcbuf, k)) js_report();
}
static void run_script(DNode *s) {
    Attr *type = attr_find(s, "type");
    if (type && !attr_is(s, "type", "text/javascript") && !attr_is(s, "type", "application/javascript") && !attr_is(s, "type", "module")) return;
    char src[URLLEN];
    DNode *save = cursorParent;
    cursorParent = s->parent;
    if (attr_c(s, "src", src, URLLEN) && src[0]) {
        char url[URLLEN]; url_join(current, src, url);
        u64 len;
        if (load_bytes(url, fetchBuf, 2 * 1024 * 1024, &len)) run_text(fetchBuf, len);
        else { scopy(status, u"スクリプトを読めません", 160); }
    } else if (s->first && s->first->type == D_TEXT) {
        int k = s->first->tlen < SRCMAX ? s->first->tlen : SRCMAX;
        if (!js_run(s->first->text, k)) js_report();
    }
    cursorParent = save;
    domDirty = 1;
}
/* イベント: 要素と、その祖先へ順に (onclick 属性・プロパティ・addEventListener) */
static int defaultPrevented;
static JsVal ev_prevent(JsVal self, JsVal *a, int n) { (void)self; (void)a; (void)n; defaultPrevented = 1; return js_undef(); }
static void fire_one(DNode *e, const char *type, JsVal ev) {
    char on[16] = "on"; ccopy(on + 2, type, 14);
    JsVal self = js_host(e);
    Attr *a = attr_find(e, on);
    if (a) { /* 属性の文字をその場で実行 (this は要素) */
        js_global("__ev", ev); js_global("__self", self);
        static c16 code[4096]; int k = 0;
        const c16 *pre = u"(function(event){"; while (*pre) code[k++] = *pre++;
        for (int i = 0; i < a->n && k < 4000; i++) code[k++] = a->v[i];
        const c16 *post = u"\n}).call(__self, __ev)"; while (*post) code[k++] = *post++;
        if (!js_run(code, k)) js_report();
    }
    JsVal *prop = !cmpi(type, "click") ? &e->onclick : !cmpi(type, "change") ? &e->onchange : !cmpi(type, "input") ? &e->oninput : 0;
    if (prop && js_is_func(*prop)) { if (!js_call(*prop, self, &ev, 1)) js_report(); }
    for (int i = 0; i < e->nlisten; i++) if (!cmpi(e->lname[i], type)) { if (!js_call(e->listen[i], self, &ev, 1)) js_report(); }
}
static void fire(DNode *e, const char *type, JsVal ev) {
    if (ev.t == JS_UNDEF) {
        ev = js_object();
        js_set(ev, "type", js_cstr(type)); js_set(ev, "target", js_host(e));
        js_set(ev, "preventDefault", js_native(ev_prevent)); js_set(ev, "stopPropagation", js_native(ev_prevent));
    }
    int bubbles = !cmpi(type, "click") || !cmpi(type, "submit") || !cmpi(type, "input") || !cmpi(type, "change");
    for (DNode *x = e; x; x = bubbles ? x->parent : 0) { if (x->type == D_ELEM || x->type == D_DOC) fire_one(x, type, ev); if (!bubbles) break; }
    domDirty = 1;
}
/* window / console / タイマー */
static JsVal w_alert(JsVal self, JsVal *a, int n) { (void)self; JsStr *s = argstr(a, n, 0); int l = s->len < 399 ? s->len : 399; for (int i = 0; i < l; i++) alertText[i] = s->s[i]; alertText[l] = 0; alertOpen = 1; return js_undef(); }
static JsVal w_confirm(JsVal self, JsVal *a, int n) { w_alert(self, a, n); return js_bool(1); }
static JsVal w_log(JsVal self, JsVal *a, int n) {
    (void)self; consoleLine[0] = 0;
    for (int i = 0; i < n; i++) { JsStr *s = js_tostr(a[i]); int l = slen(consoleLine); if (i) consoleLine[l++] = ' '; for (int j = 0; j < s->len && l < 158; j++) consoleLine[l++] = s->s[j]; consoleLine[l] = 0; }
    scopy(status, u"console: ", 160); scat(status, consoleLine, 160);
    return js_undef();
}
static JsVal add_timer(JsVal *a, int n, int repeat) {
    if (!n || !js_is_func(a[0])) return js_num(0);
    for (int i = 0; i < NTIMER; i++) if (!timers[i].used) {
        u64 ms = n > 1 ? (u64)js_tonum(a[1]) : 0; if (ms < 4) ms = 4;
        timers[i].used = 1; timers[i].fn = a[0]; timers[i].due = net_ms() + ms; timers[i].every = repeat ? ms : 0; timers[i].id = nextTimer++;
        return js_num(timers[i].id);
    }
    return js_num(0);
}
static JsVal w_setTimeout(JsVal self, JsVal *a, int n) { (void)self; return add_timer(a, n, 0); }
static JsVal w_setInterval(JsVal self, JsVal *a, int n) { (void)self; return add_timer(a, n, 1); }
static JsVal w_clear(JsVal self, JsVal *a, int n) { (void)self; int id = (int)js_tonum(n ? a[0] : js_undef()); for (int i = 0; i < NTIMER; i++) if (timers[i].used && timers[i].id == id) timers[i].used = 0; return js_undef(); }
static JsVal w_raf(JsVal self, JsVal *a, int n) { (void)self; JsVal b[2] = {n ? a[0] : js_undef(), js_num(16)}; return add_timer(b, 2, 0); }
static JsVal w_storage_get(JsVal self, JsVal *a, int n) { (void)self; (void)a; (void)n; return nul(); }
static JsVal w_matchMedia(JsVal self, JsVal *a, int n) {
    (void)self; (void)a; (void)n;
    JsVal m = js_object(); js_set(m, "matches", js_bool(0)); js_set(m, "addEventListener", js_native(e_noop)); js_set(m, "addListener", js_native(e_noop));
    return m;
}
static int lastW, lastH;
static void js_setup_page(void) {
    js_reset(jsheap, JSHEAP);
    memset(timers, 0, sizeof timers);
    alertOpen = 0; consoleLine[0] = 0;
    js_global("document", js_host(docNode));
    JsVal con = js_object();
    js_set(con, "log", js_native(w_log)); js_set(con, "error", js_native(w_log)); js_set(con, "warn", js_native(w_log)); js_set(con, "info", js_native(w_log));
    js_global("console", con);
    js_global("alert", js_native(w_alert)); js_global("confirm", js_native(w_confirm));
    js_global("setTimeout", js_native(w_setTimeout)); js_global("setInterval", js_native(w_setInterval));
    js_global("clearTimeout", js_native(w_clear)); js_global("clearInterval", js_native(w_clear));
    js_global("requestAnimationFrame", js_native(w_raf));
    js_global("addEventListener", js_native(e_listen));
    js_global("removeEventListener", js_native(e_noop));
    js_global("innerWidth", js_num(lastW ? lastW : 760)); js_global("innerHeight", js_num(lastH ? lastH : 560));
    js_global("devicePixelRatio", js_num(1));
    { /* localStorage / sessionStorage: 覚えない (ページごとに空) */
        JsVal st = js_object();
        js_set(st, "getItem", js_native(w_storage_get)); js_set(st, "setItem", js_native(e_noop)); js_set(st, "removeItem", js_native(e_noop)); js_set(st, "clear", js_native(e_noop));
        js_global("localStorage", st); js_global("sessionStorage", st);
    }
    js_global("matchMedia", js_native(w_matchMedia));
    JsVal loc = js_object(); js_set(loc, "href", js_cstr(current)); js_global("location", loc);
    JsVal nav = js_object(); js_set(nav, "userAgent", js_cstr("Mozilla/5.0 (compatible; FreeOSAny)")); js_set(nav, "language", js_cstr("ja"));
    { JsVal langs[2] = {js_cstr("ja"), js_cstr("en")}; js_set(nav, "languages", js_make_array(langs, 2)); }
    js_global("navigator", nav);
}
/* 読み込みが終わったとき */
static void page_loaded(void) {
    JsVal w = js_undef();
    fire_one(docNode, "DOMContentLoaded", w);
    fire_one(docNode, "load", w);
    JsVal onload = js_global_get("onload");
    if (js_is_func(onload)) { if (!js_call(onload, js_undef(), 0, 0)) js_report(); }
    domDirty = 1;
}

/* ==================== 並べる (DOM → 文字の列) ==================== */
#define DOCMAX (200 * 1024)
#define LINEMAX 12000
#define FOCMAX 2000
enum { S_BOLD = 1, S_LINK = 2, S_CODE = 4, S_HEAD = 8, S_ITAL = 16, S_MARK = 32, S_STRIKE = 64, S_BIG = 128 };
#define C_WIDGET 0xE000   /* 入力欄・ボタンなど (own が部品) */
#define C_INDENT 0xE001   /* この行から字下げ (sty が幅) */
#define C_RULE 0xE002     /* 横線 */
static c16 *doc; static u16 *sty; static DNode **own; static u32 *dcol;
static int dlen;
static int *lines, *lineIndent; static int nline, laidW;
static DNode *foc[FOCMAX]; static int nfoc;
static int pre, indentPx;

static void add(c16 c, u16 s, DNode *o, u32 color) {
    if (dlen >= DOCMAX) return;
    doc[dlen] = c; sty[dlen] = s; own[dlen] = o; dcol[dlen] = color; dlen++;
}
static void brk(int blank) {
    int ind = -1; /* 最後の字下げの印は改行の後ろへ回す */
    while (dlen && (doc[dlen - 1] == u' ' || doc[dlen - 1] == C_INDENT)) { if (doc[dlen - 1] == C_INDENT && ind < 0) ind = sty[dlen - 1]; dlen--; }
    if (ind >= 0) { brk(blank); if (dlen) add(C_INDENT, (u16)ind, 0, 0); return; }
    if (!dlen) return;
    if (doc[dlen - 1] != u'\n') add(u'\n', 0, 0, 0);
    if (blank && dlen > 1 && doc[dlen - 2] != u'\n') add(u'\n', 0, 0, 0);
}
static void set_indent(int px) { indentPx = px; add(C_INDENT, (u16)px, 0, 0); }
static void addstr(const c16 *s, u16 st, DNode *o, u32 col) { while (*s) add(*s++, st, o, col); }
static int is_focusable(DNode *e) {
    if (e->type != D_ELEM) return 0;
    if (is_tag(e, "a")) return attr_find(e, "href") != 0;
    if (is_tag(e, "input")) return !attr_is(e, "type", "hidden");
    return is_tag(e, "button") || is_tag(e, "select") || is_tag(e, "textarea");
}
static int listDepth, olCount[16];
static void render_node(DNode *n, u16 st, DNode *linkOwner, u32 col) {
    if (n->type == D_TEXT) {
        DNode *o = linkOwner ? linkOwner : n->parent;
        for (int i = 0; i < n->tlen; i++) {
            c16 c = n->text[i];
            if (!pre && (c == '\n' || c == '\t' || c == ' ')) { if (dlen && doc[dlen - 1] != u' ' && doc[dlen - 1] != u'\n' && doc[dlen - 1] != C_INDENT) add(u' ', st, o, col); continue; }
            if (c == '\t') c = ' ';
            if (c < 32 && c != '\n') continue;
            add(c, st, o, col);
        }
        return;
    }
    if (n->type != D_ELEM && n->type != D_DOC) return;
    if (n->hidden) return;
    const char *t = n->tag;
    if (n->type == D_ELEM) {
        if (tag_is(t, " script style head title template noscript meta link svg iframe object ")) return;
        if (attr_find(n, "hidden")) return;
        if (!n->hasColor && attr_find(n, "style")) style_attr(n);
        if (n->hidden) return;
    }
    if (n->hasColor) col = n->color;
    if (n->type == D_ELEM && !cmpi(t, "font")) { char c[32]; if (attr_c(n, "color", c, 32)) { JsVal v = js_cstr(c); u32 x = parse_color(js_tostr(v)); if (x != 0xFFFFFFFF) col = x; } }
    if (is_focusable(n) && nfoc < FOCMAX) foc[nfoc++] = n;
    int block = tag_is(t, " p div h1 h2 h3 h4 h5 h6 ul ol li table tr blockquote pre form header footer nav section article main aside figure figcaption dl dt dd hr address details summary fieldset center ");
    int blank = tag_is(t, " p h1 h2 h3 h4 h5 h6 table blockquote pre dl figure ") || (tag_is(t, " ul ol ") && listDepth == 0);
    int saveIndent = indentPx, savePre = pre;
    if (block) brk(blank && !is_tag(n, "li"));
    if (tag_is(t, " b strong th ")) st |= S_BOLD;
    if (tag_is(t, " h1 h2 h3 h4 h5 h6 ")) st |= S_BOLD | S_HEAD | (tag_is(t, " h1 h2 ") ? S_BIG : 0);
    if (tag_is(t, " i em cite var dfn ")) st |= S_ITAL;
    if (tag_is(t, " code kbd samp tt ")) st |= S_CODE;
    if (tag_is(t, " mark ")) st |= S_MARK;
    if (tag_is(t, " s del strike ")) st |= S_STRIKE;
    if (is_tag(n, "a") && attr_find(n, "href")) { st |= S_LINK; linkOwner = n; }
    if (is_tag(n, "pre")) { pre = 1; st |= S_CODE; }
    if (tag_is(t, " blockquote dd ")) { set_indent(indentPx + 24); col = col ? col : 0x55556A; }
    if (tag_is(t, " ul ol ")) { listDepth++; if (listDepth < 16) olCount[listDepth] = 0; set_indent(indentPx + 22); }
    if (is_tag(n, "li")) {
        if (indentPx < 22) set_indent(22);
        int ordered = n->parent && is_tag(n->parent, "ol");
        if (ordered && listDepth < 16) { c16 b[12] = u""; snum(b, ++olCount[listDepth], 0, 12); scat(b, u". ", 12); addstr(b, st, n, col); }
        else add(listDepth > 1 ? u'◦' : u'•', st, n, col), add(u' ', st, n, col);
    }
    if (tag_is(t, " td th ") && n->prev) add(u' ', st, n, col), add(u'│', 0, n, 0xC0C0D0), add(u' ', st, n, col);
    if (is_tag(n, "br")) { add(u'\n', 0, 0, 0); if (indentPx) set_indent(indentPx); return; }
    if (is_tag(n, "hr")) { brk(0); add(C_RULE, 0, n, 0); add(u'\n', 0, 0, 0); return; }
    if (is_tag(n, "img")) {
        Attr *alt = attr_find(n, "alt");
        add(u'[', S_ITAL, linkOwner ? linkOwner : n, 0x8888AA);
        if (alt && alt->n) for (int i = 0; i < alt->n && i < 60; i++) add(alt->v[i], S_ITAL | (st & S_LINK), linkOwner ? linkOwner : n, 0x8888AA);
        else addstr(u"画像", S_ITAL, linkOwner ? linkOwner : n, 0x8888AA);
        add(u']', S_ITAL, linkOwner ? linkOwner : n, 0x8888AA);
        return;
    }
    if (tag_is(t, " input button select textarea ")) {
        if (is_tag(n, "input") && attr_is(n, "type", "hidden")) return;
        add(C_WIDGET, st, n, col); add(u' ', st, n, col);
        return;
    }
    for (DNode *c = n->first; c; c = c->next) render_node(c, st, linkOwner, col);
    if (tag_is(t, " ul ol ")) listDepth--;
    if (indentPx != saveIndent) { brk(0); set_indent(saveIndent); }
    pre = savePre;
    if (block) brk(blank);
}
static void render_dom(void) {
    dlen = 0; nfoc = 0; pre = 0; indentPx = 0; listDepth = 0;
    if (docNode) render_node(body ? body : docNode, 0, 0, 0);
    while (dlen && (doc[dlen - 1] == u'\n' || doc[dlen - 1] == u' ')) dlen--;
    laidW = 0; domDirty = 0;
}

/* ---- 部品の大きさ ---- */
static int widget_label(DNode *e, c16 *out, int cap) {
    out[0] = 0;
    if (is_tag(e, "button")) return text_of(e, out, cap, 0);
    if (is_tag(e, "select")) { DNode *o = find_tag(e, "option"); for (DNode *x = o; x; x = x->next) if (is_tag(x, "option") && attr_find(x, "selected")) o = x; return o ? text_of(o, out, cap, 0) : 0; }
    if (is_tag(e, "input") && (attr_is(e, "type", "submit") || attr_is(e, "type", "button") || attr_is(e, "type", "reset"))) {
        if (e->text && e->tlen) { int l = e->tlen < cap - 1 ? e->tlen : cap - 1; for (int i = 0; i < l; i++) out[i] = e->text[i]; out[l] = 0; return l; }
        scopy(out, attr_is(e, "type", "submit") ? u"送信" : u"ボタン", cap); return slen(out);
    }
    return 0;
}
static int is_check(DNode *e) { return is_tag(e, "input") && (attr_is(e, "type", "checkbox") || attr_is(e, "type", "radio")); }
static int is_button(DNode *e) { return is_tag(e, "button") || (is_tag(e, "input") && (attr_is(e, "type", "submit") || attr_is(e, "type", "button") || attr_is(e, "type", "reset"))); }
static int widget_w(DNode *e) {
    if (is_check(e)) return 16;
    c16 l[80]; int n = widget_label(e, l, 80);
    if (is_button(e) || is_tag(e, "select")) { int w = 0; for (int i = 0; i < n; i++) w += charw(l[i]); return w + (is_tag(e, "select") ? 36 : 24); }
    char sz[8]; if (attr_c(e, "size", sz, 8)) { int v = 0; for (char *p = sz; *p >= '0' && *p <= '9'; p++) v = v * 10 + *p - '0'; if (v > 4 && v < 80) return v * 8 + 12; }
    return is_tag(e, "textarea") ? 320 : 200;
}
static int cw_at(int i, int avail) {
    c16 c = doc[i];
    if (c == C_WIDGET) return widget_w(own[i]);
    if (c == C_INDENT) return 0;
    if (c == C_RULE) return avail;
    return charw(c);
}
static void layout(int w) {
    if (w == laidW) return;
    laidW = w; nline = 0;
    int px = 0, ind = 0, space = -1; /* space: この行の最後の空白 (英語などは単語の切れ目で折り返す) */
    lines[0] = 0; lineIndent[0] = 0; nline = 1;
    for (int i = 0; i < dlen && nline < LINEMAX; i++) {
        if (doc[i] == u' ') space = i;
        if (doc[i] == C_INDENT) { ind = sty[i]; if (px == 0 || (i > 0 && doc[i - 1] == u'\n')) { lineIndent[nline - 1] = ind; } continue; }
        if (doc[i] == u'\n') { lines[nline] = i + 1; lineIndent[nline] = ind; nline++; px = 0; space = -1; continue; }
        int cw = cw_at(i, w - ind);
        if (px + ind + cw > w && px > 0) {
            int sp = doc[i] == u' ';
            if (!sp && doc[i] < 0x2E80 && space > lines[nline - 1] && i - space < 40) { /* 単語ごと次の行へ */
                lines[nline] = space + 1; lineIndent[nline] = ind; nline++;
                px = 0; for (int j = space + 1; j < i; j++) px += cw_at(j, w - ind);
                space = -1;
            } else { lines[nline] = sp ? i + 1 : i; lineIndent[nline] = ind; nline++; px = 0; space = -1; if (sp) continue; }
        }
        px += cw;
    }
}

/* ==================== 画面と操作 ==================== */
static int top, sel = -1, viewRows, focus = 1, editing; /* sel: 選択中の部品 (foc の番号) */
static char addr[URLLEN], history[16][URLLEN];
static int nhist;
static u8 *page; static u64 pageLen;
#define PAGEMAX (4 * 1024 * 1024)
#define LINEH 20
static int lineHeight(int l) { /* 見出し (h1/h2) の行は少し高く */
    int end = l + 1 < nline ? lines[l + 1] : dlen;
    for (int i = lines[l]; i < end; i++) if (sty[i] & S_BIG) return 26;
    return LINEH;
}

static const char home[] =
    "<h1>FreeOS Any ブラウザ</h1><p>上の欄に URL を入力して Enter。有線 LAN が必要です。HOME の .html ファイルは <code>file:名前.html</code> で開けます。</p>"
    "<h2>行き先</h2><ul><li><a href='https://example.com/'>example.com</a></li><li><a href='https://ja.wikipedia.org/wiki/オペレーティングシステム'>Wikipedia: OS</a></li>"
    "<li><a href='https://lite.duckduckgo.com/lite/'>DuckDuckGo (軽量版)</a></li><li><a href='about:demo'>HTML と JavaScript の見本</a></li></ul>"
    "<h2>操作</h2><p>Tab でリンクや部品を選ぶ、Enter で開く・押す、矢印・PageUp/Down・ホイールで動かす、BackSpace で戻る、F2 で URL 入力、s でページを保存。</p>"
    "<p>対応: 見出し・強調・リスト・表・フォーム・小さな JavaScript (DOM の書き換え、イベント、タイマー)。<br>非対応: 画像・CSS の配置・Shift_JIS など。</p>";
static const char demo[] =
    "<title>見本</title><h1>HTML と JavaScript の見本</h1>"
    "<p>これは <b>太字</b>、<i>斜体</i>、<code>コード</code>、<mark>しるし</mark>、<a href='about:home'>リンク</a> です。</p>"
    "<h2>カウンター</h2><p>押した回数: <b id='n'>0</b></p><button id='b'>押す</button> <button onclick=\"document.getElementById('n').textContent=0\">リセット</button>"
    "<h2>入力</h2><p>名前: <input id='name' placeholder='名前'> <button id='hi'>あいさつ</button></p><p id='out' style='color:#7C3AED'></p>"
    "<h2>時計</h2><p id='clock'>--:--:--</p>"
    "<h2>リストと表</h2><ol><li>一つ目</li><li>二つ目<ul><li>入れ子</li></ul></li></ol>"
    "<table><tr><th>名前</th><th>値</th></tr><tr><td>π</td><td id='pi'></td></tr><tr><td>2の10乗</td><td id='p2'></td></tr></table>"
    "<script>\n"
    "let count = 0;\n"
    "document.getElementById('b').addEventListener('click', () => { count++; document.getElementById('n').textContent = count; });\n"
    "document.getElementById('hi').onclick = function () { const v = document.getElementById('name').value || '名無し'; document.getElementById('out').textContent = `こんにちは、${v} さん！`; };\n"
    "function tick() { const d = new Date(); document.getElementById('clock').textContent = d.toLocaleTimeString() + ' (UTC)'; }\n"
    "tick(); setInterval(tick, 1000);\n"
    "document.getElementById('pi').textContent = Math.PI.toFixed(5);\n"
    "document.getElementById('p2').textContent = [1,2,3,4,5,6,7,8,9,10].reduce((a) => a * 2, 1);\n"
    "</script>";

static int init(void) {
    if (lines) return 1;
    if (BS->AllocatePool(2, DOCMAX * 2, (void **)&doc) || BS->AllocatePool(2, DOCMAX * 2, (void **)&sty) ||
        BS->AllocatePool(2, DOCMAX * sizeof(DNode *), (void **)&own) || BS->AllocatePool(2, DOCMAX * 4, (void **)&dcol) ||
        BS->AllocatePool(2, PAGEMAX, (void **)&page) || BS->AllocatePool(2, DHEAP, (void **)&dheap) ||
        BS->AllocatePool(2, JSHEAP, &jsheap) || BS->AllocatePool(2, 2 * 1024 * 1024, (void **)&fetchBuf) ||
        BS->AllocatePool(2, SRCMAX * 2, (void **)&srcbuf) || BS->AllocatePool(2, LINEMAX * sizeof(int), (void **)&lineIndent) ||
        BS->AllocatePool(2, LINEMAX * sizeof(int), (void **)&lines)) { lines = 0; return 0; }
    return 1;
}
/* HTML を読み込んで表示する (スクリプトも実行) */
static void show_html(const u8 *s, u64 n) {
    new_document();
    js_setup_page();
    status[0] = 0;
    parse_html(s, n, docNode);
    if (!body) body = find_tag(docNode, "body");
    page_loaded();
    render_dom();
    top = 0; sel = -1; editing = 0;
}
static void show_text(const u8 *s, u64 n) { /* HTML でない文字のページ */
    new_document(); js_setup_page();
    DNode *p = dnew(D_ELEM, "pre"); dappend(docNode, p);
    DNode *t = dnew(D_TEXT, 0); dappend(p, t);
    if (!t) { render_dom(); return; }
    for (u64 i = 0; i < n;) { u32 c = next_char(s, n, &i); if (c != '\r') text_add(t, (c16)c); }
    render_dom(); top = 0; sel = -1;
}
static void show_home(void) { current[0] = 0; addr[0] = 0; pageLen = 0; ccopy(current, "about:home", URLLEN); show_html((const u8 *)home, sizeof home - 1); focus = 1; }

/* URL かファイルの中身を読む */
static int load_bytes(const char *url, u8 *buf, u64 cap, u64 *len) {
    if (prefix(url, "file:")) {
        const char *p = url + 5; while (*p == '/') p++;
        c16 path[PATHLEN] = u"\\"; int k = 1;
        u64 L = (u64)clen(p);
        for (u64 j = 0; j < L && k < PATHLEN - 1;) { u32 c = next_char((const u8 *)p, L, &j); path[k++] = c == '/' ? '\\' : (c16)c; }
        path[k] = 0;
        i64 r = fs_read(path, buf, cap);
        if (r < 0) return 0;
        *len = (u64)r; return 1;
    }
    HttpResult r;
    if (!http_get(url, buf, cap, &r)) return 0;
    *len = r.len;
    return r.status == 200;
}
static void load(const char *url, int remember) {
    static char want[URLLEN], prev[URLLEN];
    if (!init()) return;
    want[0] = 0;
    if (prefix(url, "about:")) ccopy(want, url, URLLEN);
    else if (prefix(url, "file:")) ccopy(want, url, URLLEN);
    else {
        if (!prefix(url, "http://") && !prefix(url, "https://")) ccopy(want, "https://", URLLEN);
        ccopy(want + clen(want), url, URLLEN - clen(want));
    }
    ccopy(prev, current, URLLEN);
    if (remember && prev[0]) {
        if (nhist == 16) { mcopy(history[0], history[1], 15 * URLLEN); nhist--; }
        ccopy(history[nhist++], prev, URLLEN);
    }
    ccopy(addr, want, URLLEN);
    focus = 0;
    if (prefix(want, "about:demo")) { ccopy(current, want, URLLEN); show_html((const u8 *)demo, sizeof demo - 1); invalidate(); return; }
    if (prefix(want, "about:")) { show_home(); focus = 0; invalidate(); return; }
    if (prefix(want, "file:")) {
        u64 len;
        ccopy(current, want, URLLEN);
        if (!load_bytes(want, page, PAGEMAX, &len)) { scopy(status, u"ファイルを開けません", 160); invalidate(); return; }
        pageLen = len;
        int html = 0; for (const char *p = want; *p; p++) if (prefix(p, ".htm")) html = 1;
        if (html) show_html(page, len); else show_text(page, len);
        if (!status[0]) scopy(status, pageTitle, 160);
        invalidate(); return;
    }
    scopy(status, u"読み込み中… (Esc で中止)", 160);
    redraw();
    HttpResult r;
    pageLen = 0;
    if (!http_get(want, page, PAGEMAX, &r)) { scopy(status, net_error(), 160); invalidate(); return; }
    ccopy(current, r.url, URLLEN); ccopy(addr, r.url, URLLEN);
    pageLen = r.len;
    char type[80]; int i = 0;
    for (; r.type[i]; i++) type[i] = lower(r.type[i]);
    type[i] = 0;
    int html = !type[0], text = 0, sjis = 0;
    for (char *p = type; *p; p++) { if (prefix(p, "html")) html = 1; if (prefix(p, "text/") || prefix(p, "json") || prefix(p, "xml")) text = 1; if (prefix(p, "shift_jis") || prefix(p, "euc-jp")) sjis = 1; }
    if (html) show_html(page, r.len);
    else if (text) show_text(page, r.len);
    else { static const char m[] = "<p>このファイルは表示できません。s キーで保存できます。</p>"; show_html((const u8 *)m, sizeof m - 1); }
    if (!status[0]) {
        if (r.status != 200) { scat(status, u"HTTP ", 160); snum(status, r.status, 0, 160); scat(status, u"  ", 160); }
        if (sjis) scat(status, u"未対応の文字コードのため文字化けします  ", 160);
        if (r.truncated) scat(status, u"⚠ 途中で切れています", 160);
        if (!status[0]) scopy(status, pageTitle, 160);
    }
    invalidate();
}
void browser_go(const char *url) { wm_open(APP_BROWSER); load(url, 1); }
void browser_open_file(const c16 *path) {
    static char u[URLLEN]; int k = 0;
    ccopy(u, "file:", URLLEN); k = 5;
    for (int i = 0; path[i] && k < URLLEN - 4; i++) {
        c16 c = path[i] == '\\' ? '/' : path[i];
        if (c < 0x80) u[k++] = (char)c;
        else if (c < 0x800) { u[k++] = 0xC0 | c >> 6; u[k++] = 0x80 | (c & 63); }
        else { u[k++] = 0xE0 | c >> 12; u[k++] = 0x80 | ((c >> 6) & 63); u[k++] = 0x80 | (c & 63); }
    }
    u[k] = 0;
    browser_go(u);
}


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


/* ---- フォームの送信 (GET) ---- */
static void urlenc(char *out, int *k, int cap, const c16 *s, int n) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < n && *k < cap - 10; i++) {
        c16 c = s[i]; u8 b[3]; int m = 0;
        if (c < 0x80) b[m++] = (u8)c; else if (c < 0x800) b[m++] = 0xC0 | c >> 6, b[m++] = 0x80 | (c & 63);
        else b[m++] = 0xE0 | c >> 12, b[m++] = 0x80 | ((c >> 6) & 63), b[m++] = 0x80 | (c & 63);
        for (int j = 0; j < m; j++) {
            u8 x = b[j];
            if ((x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z') || (x >= '0' && x <= '9') || x == '-' || x == '_' || x == '.' || x == '~') out[(*k)++] = (char)x;
            else if (x == ' ') out[(*k)++] = '+';
            else { out[(*k)++] = '%'; out[(*k)++] = hex[x >> 4]; out[(*k)++] = hex[x & 15]; }
        }
    }
}
static void collect_form(DNode *n, char *q, int *k, int cap, DNode *submitter) {
    for (DNode *c = n->first; c; c = c->next) {
        if (c->type != D_ELEM) continue;
        Attr *name = attr_find(c, "name");
        if (name && name->n && (is_tag(c, "input") || is_tag(c, "textarea") || is_tag(c, "select") || is_tag(c, "button"))) {
            int skip = 0; c16 val[256]; int vl = 0;
            if (is_check(c)) { if (!c->checked) skip = 1; else { Attr *v = attr_find(c, "value"); if (v) { vl = v->n < 255 ? v->n : 255; for (int i = 0; i < vl; i++) val[i] = v->v[i]; } else { val[0] = 'o'; val[1] = 'n'; vl = 2; } } }
            else if (is_button(c)) { if (c != submitter) skip = 1; else vl = widget_label(c, val, 256); }
            else if (is_tag(c, "select")) { DNode *o = find_tag(c, "option"); for (DNode *x = o; x; x = x->next) if (is_tag(x, "option") && attr_find(x, "selected")) o = x; Attr *v = o ? attr_find(o, "value") : 0; if (v) { vl = v->n < 255 ? v->n : 255; for (int i = 0; i < vl; i++) val[i] = v->v[i]; } else if (o) vl = text_of(o, val, 256, 0); }
            else { vl = c->tlen < 255 ? c->tlen : 255; for (int i = 0; i < vl; i++) val[i] = c->text[i]; }
            if (!skip) {
                if (*k && q[*k - 1] != '?') q[(*k)++] = '&';
                urlenc(q, k, cap, name->v, name->n); q[(*k)++] = '='; urlenc(q, k, cap, val, vl);
            }
        }
        collect_form(c, q, k, cap, submitter);
    }
}
static void submit(DNode *from) {
    DNode *f = from; while (f && !is_tag(f, "form")) f = f->parent;
    if (!f) return;
    defaultPrevented = 0;
    fire(f, "submit", js_undef());
    if (defaultPrevented) { domDirty = 1; return; }
    static char q[URLLEN * 2]; char action[URLLEN];
    if (!attr_c(f, "action", action, URLLEN) || !action[0]) ccopy(action, current, URLLEN);
    char base[URLLEN]; url_join(current, action, base);
    int k = 0; for (; base[k] && base[k] != '?' && base[k] != '#'; k++) q[k] = base[k];
    q[k++] = '?';
    collect_form(f, q, &k, URLLEN, is_button(from) ? from : 0);
    q[k < URLLEN ? k : URLLEN - 1] = 0;
    if (attr_is(f, "method", "post")) scopy(status, u"POST は GET で送ります", 160);
    load(q, 1);
}
/* 部品やリンクを押したとき */
static void activate(DNode *e) {
    if (!e) return;
    defaultPrevented = 0;
    if (is_check(e)) {
        if (attr_is(e, "type", "radio")) { /* 同じ name の他を外す */
            char nm[64]; attr_c(e, "name", nm, 64);
            for (int i = 0; i < nfoc; i++) { char x[64]; if (is_check(foc[i]) && attr_c(foc[i], "name", x, 64) && !cmpi(x, nm)) foc[i]->checked = 0; }
            e->checked = 1;
        } else e->checked ^= 1;
        fire(e, "click", js_undef()); fire(e, "change", js_undef());
        return;
    }
    fire(e, "click", js_undef());
    if (defaultPrevented) return;
    if (is_button(e) && !attr_is(e, "type", "button") && !(is_tag(e, "button") && attr_is(e, "type", "button"))) {
        DNode *f = e; while (f && !is_tag(f, "form")) f = f->parent;
        if (f && (is_tag(e, "button") || attr_is(e, "type", "submit"))) { submit(e); return; }
    }
    for (DNode *a = e; a; a = a->parent) if (is_tag(a, "a") && attr_find(a, "href")) {
        char h[URLLEN], u[URLLEN];
        attr_c(a, "href", h, URLLEN);
        if (h[0] == '#' || prefix(h, "javascript:")) return;
        url_join(current, h, u);
        load(u, 1);
        return;
    }
}
static void back(void) {
    if (!nhist) { show_home(); return; }
    static char u[URLLEN]; ccopy(u, history[--nhist], URLLEN); load(u, 0);
}
static void scroll_to_sel(void) {
    if (sel < 0) return;
    for (int l = 0; l < nline; l++) {
        int end = l + 1 < nline ? lines[l + 1] : dlen;
        for (int i = lines[l]; i < end; i++) if (own[i] == foc[sel] || (own[i] && own[i]->parent == foc[sel])) { if (l < top || l >= top + viewRows) top = l > 2 ? l - 2 : 0; return; }
    }
}
static int text_widget(DNode *e) { return e && (is_tag(e, "textarea") || (is_tag(e, "input") && !is_check(e) && !is_button(e))); }

void browser_open(void) { if (init() && !current[0]) show_home(); }
void browser_tick(void) {
    if (!lines || !docNode) return;
    u64 now = net_ms(); int ran = 0;
    for (int i = 0; i < NTIMER && ran < 4; i++) if (timers[i].used && now >= timers[i].due) { /* 1 回に 4 つまで */
        JsVal f = timers[i].fn;
        if (timers[i].every) timers[i].due = now + (timers[i].every < 50 ? 50 : timers[i].every); else timers[i].used = 0;
        if (!js_call(f, js_undef(), 0, 0)) { js_report(); timers[i].used = 0; } /* エラーになったタイマーは止める */
        ran++;
    }
    if (ran || domDirty) { if (domDirty) render_dom(); invalidate(); }
}
void browser_wheel(int dy) { if (lines) { top += dy * 3; invalidate(); } }
void browser_key(Key k) {
    if (!lines) return;
    if (alertOpen) { if (k.ch == 13 || k.ch == 27 || k.ch == ' ') alertOpen = 0; return; }
    if (k.scan == 0x0C) { focus = 1; editing = 0; return; } /* F2 */
    int n = clen(addr);
    if (focus) {
        if (k.ch == 13) { if (n) load(addr, 1); }
        else if (k.ch == 8) { if (n) addr[n - 1] = 0; }
        else if (k.scan == 8) addr[0] = 0;
        else if (k.ch == 9 || k.scan == 2) focus = 0;
        else if (k.ch > 32 && k.ch < 127 && n < URLLEN - 1) addr[n] = (char)k.ch, addr[n + 1] = 0;
        return;
    }
    DNode *s = sel >= 0 && sel < nfoc ? foc[sel] : 0;
    if (editing && text_widget(s)) { /* 入力欄に文字を入れる */
        if (k.ch == 13) {
            if (is_tag(s, "textarea")) text_add(s, u'\n');
            else { fire(s, "change", js_undef()); editing = 0; submit(s); return; }
        } else if (k.ch == 8) { if (s->tlen) s->text[--s->tlen] = 0; }
        else if (k.ch == 9) { editing = 0; sel = (sel + 1) % (nfoc ? nfoc : 1); scroll_to_sel(); return; }
        else if (k.ch >= 32) text_add(s, k.ch);
        else return;
        fire(s, "input", js_undef());
        if (domDirty) render_dom();
        return;
    }
    if (k.scan == 1) top--;
    else if (k.scan == 2) top++;
    else if (k.scan == 9) top -= viewRows - 1;
    else if (k.scan == 10 || k.ch == u' ') top += viewRows - 1;
    else if (k.scan == 5) top = 0;
    else if (k.scan == 6) top = nline;
    else if (k.ch == 9 && nfoc) { sel = (sel + 1) % nfoc; scroll_to_sel(); editing = text_widget(foc[sel]); }
    else if (k.ch == 13 && s) { if (text_widget(s)) editing = 1; else activate(s); }
    else if (k.ch == 8) back();
    else if (k.ch == u's' && current[0] && pageLen) {
        c16 path[PATHLEN]; path[0] = 0;
        save_name(current, path);
        if (fs_write(path, page, pageLen)) { scopy(status, u"保存しました: ", 160); scat(status, path + 1, 160); }
        else scopy(status, u"保存できません", 160);
    }
    if (domDirty) render_dom();
}

/* ---- 描く ---- */
#define BAR 30
#define SCROLLW 12
static int sbDrag = -1; /* つまみをドラッグ中 (未使用: 予約) */
static void draw_widget(int x, int y, DNode *e, int on) {
    int w = widget_w(e);
    if (is_check(e)) {
        int r = attr_is(e, "type", "radio");
        fill(x, y + 2, 14, 14, on ? 0x6D5DFC : 0x8888A0); fill(x + 1, y + 3, 12, 12, 0xFFFFFF);
        if (r) { fill(x + 1, y + 3, 2, 2, 0xFFFFFF); }
        if (e->checked) fill(x + 4, y + 6, 6, 6, 0x6D5DFC);
        return;
    }
    c16 l[80]; widget_label(e, l, 80);
    if (is_button(e)) {
        fill(x, y, w, 19, on ? 0x5B49EE : 0x6D5DFC); fill(x, y + 18, w, 1, 0x3A2FA0);
        text(x + 12, y + 1, l, 0xFFFFFF);
        return;
    }
    if (is_tag(e, "select")) {
        fill(x, y, w, 19, on ? 0x6D5DFC : 0xB0B0C8); fill(x + 1, y + 1, w - 2, 17, 0xFFFFFF);
        text(x + 8, y + 1, l, 0x1D1D27); text(x + w - 18, y + 1, u"▾", 0x55556A);
        return;
    }
    fill(x, y, w, 19, on ? 0x6D5DFC : 0xB0B0C8); fill(x + 1, y + 1, w - 2, 17, 0xFFFFFF);
    clip(x + 2, y, w - 4, 19);
    if (e->tlen) {
        int tw = 0; for (int i = 0; i < e->tlen; i++) tw += charw(e->text[i]);
        int cx = x + 6 - (tw > w - 16 ? tw - (w - 16) : 0);
        int pw = attr_is(e, "type", "password");
        for (int i = 0; i < e->tlen; i++) cx += glyph(cx, y + 1, pw ? u'●' : e->text[i], 0x1D1D27);
        if (on && editing) fill(cx + 1, y + 2, 2, 15, 0x6D5DFC);
    } else {
        Attr *ph = attr_find(e, "placeholder");
        if (ph) { int cx = x + 6; for (int i = 0; i < ph->n; i++) cx += glyph(cx, y + 1, ph->v[i], 0xA0A0B8); }
        if (on && editing) fill(x + 6, y + 2, 2, 15, 0x6D5DFC);
    }
    noclip();
}
static int content_h(void) { int h = 0; for (int l = 0; l < nline; l++) h += lineHeight(l); return h; }
void browser_draw(int x, int y, int w, int h) {
    lastW = w; lastH = h;
    fill(x, y, w, h, 0xFFFFFF);
    if (!lines) { text(x + 8, y + 8, u"メモリが足りません", 0); return; }
    if (domDirty) render_dom();
    fill(x, y, w, BAR, 0xE4E4F2);
    text(x + 10, y + 7, u"←", 0x33334A);
    fill(x + 30, y + 4, w - 36, 22, focus ? 0xFFFFFF : 0xF4F4FA);
    if (focus) fill(x + 30, y + 25, w - 36, 2, 0x6D5DFC);
    /* URL は UTF-8 (と %xx) をほどいて表示する。長ければ後ろを見せる */
    static c16 shown[URLLEN]; int sn = 0; u64 L = (u64)clen(addr);
    for (u64 i = 0; i < L && sn < URLLEN - 1;) {
        if (addr[i] == '%' && i + 2 < L) { /* %E3%81%82 → あ */
            u8 b[4]; int nb = 0; u64 j = i;
            while (j + 2 < L && addr[j] == '%' && nb < 4) { int hi = addr[j + 1], lo = addr[j + 2]; hi = hi <= '9' ? hi - '0' : (hi | 32) - 'a' + 10; lo = lo <= '9' ? lo - '0' : (lo | 32) - 'a' + 10; if (hi < 0 || hi > 15 || lo < 0 || lo > 15) break; b[nb++] = (u8)(hi << 4 | lo); j += 3; if ((b[0] & 0xC0) != 0xC0) break; int need = b[0] >= 0xF0 ? 4 : b[0] >= 0xE0 ? 3 : 2; if (nb == need) break; }
            if (nb && (b[0] < 0x80 || (nb >= 2 && nb == (b[0] >= 0xF0 ? 4 : b[0] >= 0xE0 ? 3 : 2)))) { u64 k = 0; shown[sn++] = (c16)next_char(b, (u64)nb, &k); i = j; continue; }
        }
        shown[sn++] = (c16)next_char((const u8 *)addr, L, &i);
    }
    shown[sn] = 0;
    int px = x + 36, avail = w - 56, from = 0, tw = 0;
    for (int i = 0; i < sn; i++) tw += charw(shown[i]);
    while (tw > avail && from < sn) tw -= charw(shown[from++]);
    for (int i = from; i < sn; i++) px += glyph(px, y + 7, shown[i], 0x1D1D27);
    if (focus) fill(px + 1, y + 7, 2, 16, 0x6D5DFC);
    int width = w - 24 - SCROLLW;
    layout(width);
    int viewH = h - BAR - 26;
    viewRows = viewH / LINEH;
    if (top > nline - viewRows) top = nline - viewRows;
    if (top < 0) top = 0;
    DNode *s = sel >= 0 && sel < nfoc ? foc[sel] : 0;
    clip(x, y + BAR, w - SCROLLW, viewH + 4);
    int cy = y + BAR + 4;
    for (int l = top; l < nline && cy < y + BAR + viewH; l++) {
        int end = l + 1 < nline ? lines[l + 1] : dlen, lh = lineHeight(l), cx = x + 10 + lineIndent[l];
        int base = cy + (lh - 18);
        for (int i = lines[l]; i < end; i++) {
            c16 c = doc[i];
            if (c == u'\n' || c == C_INDENT) continue;
            int cw = cw_at(i, width - lineIndent[l]);
            if (c == C_RULE) { fill(cx, base + 8, cw - 10, 1, 0xC8C8D8); cx += cw; continue; }
            DNode *o = own[i];
            int on = s && (o == s || (o && o->parent == s));
            if (c == C_WIDGET) { draw_widget(cx, base, o, on); cx += cw; continue; }
            u16 st = sty[i];
            u32 col = dcol[i] ? dcol[i] : st & S_LINK ? 0x2455C8 : st & S_HEAD ? 0x2A1F80 : st & S_ITAL ? 0x55556A : 0x1D1D27;
            if (st & S_CODE) fill(cx, base, cw, 18, 0xEFEFF6);
            if (st & S_MARK) fill(cx, base, cw, 18, 0xFFF1A8);
            if (on && (st & S_LINK)) { fill(cx, base, cw, 18, 0x6D5DFC); col = 0xFFFFFF; }
            glyph(cx, base + 1, c, col);
            if (st & S_BOLD) glyph(cx + 1, base + 1, c, col);
            if (st & S_LINK) fill(cx, base + 17, cw, 1, col);
            if (st & S_STRIKE) fill(cx, base + 9, cw, 1, col);
            cx += cw;
        }
        /* 見出しの下線 */
        if (lh > LINEH) fill(x + 10, cy + lh - 1, width - 10, 1, 0xDDDDEA);
        cy += lh;
    }
    noclip();
    /* スクロールバー */
    int total = content_h(), sx = x + w - SCROLLW, sy = y + BAR, sh = viewH + 4;
    fill(sx, sy, SCROLLW, sh, 0xF0F0F6);
    if (total > viewH) {
        int above = 0; for (int l = 0; l < top; l++) above += lineHeight(l);
        int th = sh * viewH / total; if (th < 24) th = 24;
        int ty = sy + (sh - th) * above / (total - viewH > 0 ? total - viewH : 1);
        if (ty + th > sy + sh) ty = sy + sh - th;
        fill(sx + 3, ty + 2, SCROLLW - 6, th - 4, sbDrag >= 0 ? 0x6D5DFC : 0xB8B8CC);
    }
    /* 状態 */
    fill(x, y + h - 22, w, 22, 0xE4E4F2);
    if (s && !focus && (is_tag(s, "a"))) { char hr[URLLEN], u[URLLEN]; attr_c(s, "href", hr, URLLEN); url_join(current, hr, u); int cx = x + 8; for (int i = 0; u[i] && cx < x + w - 16; i++) cx += glyph(cx, y + h - 19, (u8)u[i], 0x2455C8); }
    else text(x + 8, y + h - 19, status, 0x55556A);
    /* alert */
    if (alertOpen) {
        int bw = w - 80 < 420 ? w - 80 : 420, bx = x + (w - bw) / 2, by = y + h / 3;
        fill(bx + 4, by + 4, bw, 110, 0x9090A8);
        fill(bx, by, bw, 110, 0xFFFFFF); fill(bx, by, bw, 26, 0x6D5DFC);
        text(bx + 10, by + 5, u"ページからのお知らせ", 0xFFFFFF);
        clip(bx + 10, by + 30, bw - 20, 40); text(bx + 12, by + 40, alertText, 0x1D1D27); noclip();
        fill(bx + bw - 80, by + 78, 64, 22, 0x6D5DFC); text(bx + bw - 60, by + 81, u"OK", 0xFFFFFF);
    }
}
/* (x, y) にある文字の番号 */
static int char_at(int x, int y, int w) {
    int cy = BAR + 4, width = w - 24 - SCROLLW;
    for (int l = top; l < nline; l++) {
        int lh = lineHeight(l);
        if (y >= cy && y < cy + lh) {
            int end = l + 1 < nline ? lines[l + 1] : dlen, cx = 10 + lineIndent[l];
            for (int i = lines[l]; i < end; i++) {
                if (doc[i] == u'\n' || doc[i] == C_INDENT) continue;
                int cw = cw_at(i, width - lineIndent[l]);
                if (x >= cx && x < cx + cw) return i;
                cx += cw;
            }
            return -1;
        }
        cy += lh;
    }
    return -1;
}
void browser_click(int x, int y) {
    if (!lines) return;
    if (alertOpen) { alertOpen = 0; return; }
    if (y < BAR) { if (x < 30) back(); else focus = 1; editing = 0; return; }
    focus = 0;
    int w = lastW ? lastW : 718;
    if (x >= w - SCROLLW) { /* スクロールバー: 押した所へ */
        int total = content_h(), viewH = (lastH ? lastH : 520) - BAR - 26;
        if (total > viewH) { int target = (y - BAR) * total / (viewH + 4) - viewH / 2, acc = 0, l = 0; while (l < nline && acc < target) acc += lineHeight(l++); top = l; }
        return;
    }
    int i = char_at(x, y, w);
    if (i < 0) { editing = 0; return; }
    DNode *o = own[i];
    for (int k = 0; k < nfoc; k++) if (foc[k] == o || (o && o->parent == foc[k]) ) { sel = k; break; }
    if (doc[i] == C_WIDGET && text_widget(o)) { editing = 1; for (int k = 0; k < nfoc; k++) if (foc[k] == o) sel = k; return; }
    editing = 0;
    activate(o);
    if (domDirty) render_dom();
}
