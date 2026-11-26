/* FreeOS Any 標準アプリ: ターミナル、ファイル、メモ帳、情報 */
#include "os.h"
#include "net.h"

enum { K_UP = 1, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END, K_INS, K_DEL, K_PGUP, K_PGDN, K_F2 = 0x0C, K_F3 = 0x0D };

/* ==================== ターミナル ==================== */
#define TLINES 200
#define TCOLS 128
static c16 tbuf[TLINES][TCOLS];
static int tcount;
static c16 cmd[TCOLS], lastcmd[TCOLS], cwd[PATHLEN];
static int ncmd;
static u8 iobuf[96 * 1024];
static c16 wbuf[32 * 1024];

static void newline(void) {
    if (tcount == TLINES) mcopy(tbuf[0], tbuf[1], sizeof tbuf - sizeof tbuf[0]), tcount--;
    tbuf[tcount++][0] = 0;
}
static void print(const c16 *s) {
    newline();
    for (int n = 0; *s; s++) {
        if (*s == u'\r') continue;
        if (*s == u'\n') { if (s[1]) newline(), n = 0; continue; }
        if (n >= 70) newline(), n = 0;
        tbuf[tcount - 1][n++] = *s == u'\t' ? u' ' : *s;
        tbuf[tcount - 1][n] = 0;
    }
}

/* 四則演算 (+ - * / と括弧) */
static const c16 *cp;
static int cerr, depth;
static double expr(void);
static void ws(void) { while (*cp == u' ') cp++; }
static double atom(void) {
    ws();
    if (++depth > 50) { cerr = 1; return 0; }
    double v = 0;
    if (*cp == u'-') { cp++; v = -atom(); }
    else if (*cp == u'(') { cp++; v = expr(); ws(); if (*cp == u')') cp++; else cerr = 1; }
    else {
        if (*cp < u'0' || *cp > u'9') cerr = 1;
        while (*cp >= u'0' && *cp <= u'9') v = v * 10 + (*cp++ - u'0');
        if (*cp == u'.') { double f = 0.1; for (cp++; *cp >= u'0' && *cp <= u'9'; f /= 10) v += (*cp++ - u'0') * f; }
    }
    depth--;
    return v;
}
static double term(void) {
    double v = atom();
    for (;;) {
        ws();
        if (*cp == u'*') { cp++; v *= atom(); }
        else if (*cp == u'/') { cp++; double r = atom(); if (r == 0) cerr = 1; else v /= r; }
        else return v;
    }
}
static double expr(void) {
    double v = term();
    for (;;) {
        ws();
        if (*cp == u'+') { cp++; v += term(); }
        else if (*cp == u'-') { cp++; v -= term(); }
        else return v;
    }
}
static void calc(const c16 *s) {
    cp = s; cerr = depth = 0;
    double v = expr(); ws();
    if (cerr || *cp || !(v < 1e15 && v > -1e15)) { print(u"計算できません"); return; }
    c16 out[64] = u"= ";
    if (v < 0) scat(out, u"-", 64), v = -v;
    i64 i = (i64)v, f = (i64)((v - (double)i) * 1000000 + 0.5);
    if (f >= 1000000) i++, f -= 1000000;
    snum(out, i, 0, 64);
    if (f) { int w = 6; while (f % 10 == 0) f /= 10, w--; scat(out, u".", 64); snum(out, f, w, 64); }
    print(out);
}

static u64 total_memory(void) {
    static u8 map[64 * 1024];
    u64 size = sizeof map, key, dsize; u32 ver;
    if (BS->GetMemoryMap(&size, map, &key, &dsize, &ver)) return 0;
    u64 pages = 0;
    for (u64 off = 0; off < size; off += dsize) {
        MemoryDesc *d = (MemoryDesc *)(map + off);
        if (d->type >= 1 && d->type <= 10 && d->type != 8) pages += d->pages;
    }
    return pages * 4096 / (1024 * 1024);
}

static void run(c16 *line) {
    c16 *arg = line;
    while (*arg && *arg != u' ') arg++;
    if (*arg) *arg++ = 0;
    while (*arg == u' ') arg++;
    c16 path[PATHLEN]; Entry e;
    int isfile = seq(line, u"ls") || seq(line, u"cd") || seq(line, u"cat") || seq(line, u"mkdir") || seq(line, u"edit") || seq(line, u"rm");

    if (!*line) return;
    if (seq(line, u"help")) {
        print(u"ls [dir]      一覧           cd <dir>      移動\n"
              u"cat <file>    表示           edit <file>   編集\n"
              u"mkdir <dir>   フォルダ作成   rm <path>     ゴミ箱へ\n"
              u"write <file> <text>  1行書く calc <式>     計算\n"
              u"get <URL> [名前]  ダウンロード   web <URL>  ブラウザで開く\n"
              u"net date mem cls files solitaire about reboot shutdown");
    } else if (seq(line, u"calc")) calc(arg);
    else if (seq(line, u"cls")) tcount = 0;
    else if (seq(line, u"files")) wm_open(APP_FILES);
    else if (seq(line, u"about")) wm_open(APP_ABOUT);
    else if (seq(line, u"solitaire")) wm_open(APP_SOLITAIRE);
    else if (seq(line, u"net")) {
        print(u"接続中… (Esc で中止)"); redraw();
        if (!net_up()) { print(net_error()); return; }
        c16 s[64] = u"IPアドレス: ", ip[24]; net_ipstr(net_ip(), ip, 24); scat(s, ip, 64); print(s);
    } else if (seq(line, u"get") || seq(line, u"web")) {
        static char url[URLLEN];
        int n = 0;
        while (arg[n] && arg[n] != u' ') n++;
        c16 *name = arg + n;
        if (*name) *name++ = 0;
        if (!n || n * 3 >= URLLEN) { print(u"使い方: get <URL> [名前] / web <URL>"); return; }
        url[utf8_encode(arg, n, (u8 *)url)] = 0;
        if (seq(line, u"web")) { browser_go(url); return; }
        path[0] = 0;
        if (*name && !fs_resolve(cwd, name, path)) { print(u"その名前は使えません"); return; }
        if (path[0] && fs_stat(path, 0)) { print(u"同じ名前がすでにあります"); return; }
        print(u"ダウンロード中… (Esc で中止)"); redraw();
        if (!download(url, path)) { print(net_error()); return; }
        c16 s[PATHLEN + 16] = u"保存しました: "; scat(s, path, PATHLEN + 16); print(s);
    }
    else if (seq(line, u"reboot")) power(1);
    else if (seq(line, u"shutdown")) power(0);
    else if (seq(line, u"mem")) { c16 s[64] = u"使えるメモリ: "; snum(s, total_memory(), 0, 64); scat(s, u" MB", 64); print(s); }
    else if (seq(line, u"date")) {
        Time t; c16 s[64] = u"";
        if (ST->rt->GetTime(&t, 0)) { print(u"時計を読めません"); return; }
        snum(s, t.year, 4, 64); scat(s, u"-", 64); snum(s, t.month, 2, 64); scat(s, u"-", 64); snum(s, t.day, 2, 64); scat(s, u" ", 64);
        snum(s, t.hour, 2, 64); scat(s, u":", 64); snum(s, t.minute, 2, 64); scat(s, u":", 64); snum(s, t.second, 2, 64);
        print(s);
    } else if (seq(line, u"write")) {
        c16 *body = arg;
        while (*body && *body != u' ') body++;
        if (*body) *body++ = 0;
        if (!*arg || !fs_resolve(cwd, arg, path)) { print(u"使い方: write <file> <text>"); return; }
        if (fs_stat(path, 0)) { print(u"すでにあります。edit で編集してください"); return; }
        int n = utf8_encode(body, slen(body), iobuf);
        iobuf[n++] = '\n';
        if (!fs_write(path, iobuf, n)) print(u"書き込めません");
    } else if (isfile) {
        if (!fs_ready()) { print(u"保存先がありません"); return; }
        if (!*arg && !seq(line, u"ls") && !seq(line, u"cd")) { print(u"名前を指定してください"); return; }
        if (!fs_resolve(cwd, *arg ? arg : seq(line, u"cd") ? u"\\" : u".", path)) { print(u"その名前は使えません"); return; }
        int found = fs_stat(path, &e), dir = !path[0] || (found && e.dir);
        if (seq(line, u"ls")) {
            static Entry list[MAXLIST];
            if (!dir) { print(found ? e.name : u"ありません"); return; }
            int n = fs_list(path, list, MAXLIST);
            if (n <= 0) { print(n ? u"読めません" : u"（空）"); return; }
            for (int i = 0; i < n; i++) {
                c16 s[100]; scopy(s, list[i].name, 100);
                if (list[i].dir) scat(s, u"\\", 100);
                else { scat(s, u"  (", 100); snum(s, list[i].size, 0, 100); scat(s, u" B)", 100); }
                print(s);
            }
        } else if (seq(line, u"cd")) {
            if (dir) scopy(cwd, path, PATHLEN); else print(u"フォルダがありません");
        } else if (seq(line, u"cat")) {
            i64 n = fs_read(path, iobuf, sizeof iobuf);
            int m = n < 0 ? -1 : utf8_decode(iobuf, n, wbuf, 32 * 1024 - 1);
            if (m < 0) { print(u"読めません"); return; }
            wbuf[m] = 0; print(wbuf);
        } else if (seq(line, u"mkdir")) {
            if (!fs_mkdir(path)) print(u"作れません");
        } else if (seq(line, u"edit")) {
            if (dir) print(u"フォルダは編集できません"); else edit_open(path);
        } else {
            int n = slen(path), m = slen(cwd), inside = m >= n;
            for (int i = 0; inside && i < n; i++) inside = cwd[i] == path[i];
            if (inside && (cwd[n] == 0 || cwd[n] == u'\\')) print(u"今いるフォルダは削除できません");
            else print(found && fs_trash(path) ? u"ゴミ箱に移動しました" : u"削除できません");
        }
    } else print(u"コマンドがありません。help で一覧を表示します");
}

static void term_open(void) { if (!tcount) print(u"FreeOS Any ターミナル — help でコマンド一覧"); }
static void term_key(Key k) {
    if (k.ch == 13) {
        c16 s[PATHLEN + TCOLS] = u"";
        scat(s, cwd, PATHLEN + TCOLS); scat(s, u"> ", PATHLEN + TCOLS); scat(s, cmd, PATHLEN + TCOLS);
        print(s);
        if (ncmd) scopy(lastcmd, cmd, TCOLS);
        c16 line[TCOLS]; scopy(line, cmd, TCOLS);
        cmd[ncmd = 0] = 0;
        run(line);
    } else if (k.ch == 8) { if (ncmd) cmd[--ncmd] = 0; }
    else if (k.scan == K_UP) { scopy(cmd, lastcmd, TCOLS); ncmd = slen(cmd); }
    else if (k.ch >= 32 && ncmd < 68) { cmd[ncmd++] = k.ch; cmd[ncmd] = 0; }
}
static void term_draw(int x, int y, int w, int h) {
    fill(x, y, w, h, 0x0F1117);
    int rows = (h - 12) / 18 - 1;
    int first = tcount > rows ? tcount - rows : 0, py = y + 6;
    for (int i = first; i < tcount; i++, py += 18) text(x + 8, py, tbuf[i], 0xD7E0EA);
    int px = text(x + 8, py, cwd, 0x6EE7A8);
    px = text(px, py, u"> ", 0x6EE7A8);
    px = text(px, py, cmd, 0xFFFFFF);
    fill(px + 1, py, 8, 16, 0x6EE7A8);
}

/* ==================== ファイル ==================== */
static Entry flist[MAXLIST];
static int fcount, fsel, ftop;
static c16 fdir[PATHLEN];

static void files_load(void) {
    fcount = fs_list(fdir, flist, MAXLIST);
    if (fcount < 0) fdir[0] = 0, fcount = fs_list(fdir, flist, MAXLIST);
    if (fcount < 0) fcount = 0;
    if (fsel >= fcount) fsel = fcount ? fcount - 1 : 0;
}
static int selected(c16 *path) { return fsel < fcount && fs_resolve(fdir, flist[fsel].name, path); }
static int in_trash(void) { const c16 *t = u"\\TRASH"; int i = 0; while (t[i] && fdir[i] == t[i]) i++; return !t[i]; }
static void files_enter(void) {
    c16 path[PATHLEN];
    if (!selected(path)) return;
    if (flist[fsel].dir) { scopy(fdir, path, PATHLEN); fsel = ftop = 0; files_load(); }
    else edit_open(path);
}
static void files_key(Key k) {
    c16 path[PATHLEN];
    files_load();
    if (k.scan == K_UP && fsel) fsel--;
    else if (k.scan == K_DOWN && fsel + 1 < fcount) fsel++;
    else if (k.ch == 13) files_enter();
    else if (k.ch == 8) { fs_resolve(fdir, u"..", path); scopy(fdir, path, PATHLEN); fsel = ftop = 0; files_load(); }
    else if (k.scan == K_DEL && selected(path)) {
        if (in_trash()) notify(u"ゴミ箱の中です。R キーで元の場所(一番上)に戻せます");
        else notify(fs_trash(path) ? u"ゴミ箱に移動しました" : u"削除できません");
        files_load();
    } else if ((k.ch == u'r' || k.ch == u'R') && in_trash() && selected(path)) {
        c16 to[PATHLEN];
        notify(fs_resolve(u"", flist[fsel].name, to) && fs_move(path, to) ? u"一番上のフォルダに戻しました" : u"戻せません（同じ名前があります）");
        files_load();
    }
}
static void files_draw(int x, int y, int w, int h) {
    c16 s[PATHLEN + 16] = u"HOME";
    scat(s, fdir, PATHLEN + 16);
    fill(x, y, w, 26, 0xE4E4F2);
    text(x + 8, y + 5, s, 0x33334A);
    int rows = (h - 52) / 20;
    if (fsel < ftop) ftop = fsel;
    if (fsel >= ftop + rows) ftop = fsel - rows + 1;
    for (int i = ftop; i < fcount && i < ftop + rows; i++) {
        int py = y + 28 + (i - ftop) * 20;
        if (i == fsel) fill(x + 2, py, w - 4, 20, 0x6D5DFC);
        u32 c = i == fsel ? 0xFFFFFF : 0x1D1D27;
        text(x + 10, py + 2, flist[i].dir ? u"■" : u"□", c);
        text(x + 34, py + 2, flist[i].name, c);
    }
    if (!fcount) text(x + 10, y + 32, fs_ready() ? u"空です" : u"保存先がありません", 0x6B6B7B);
    fill(x, y + h - 22, w, 22, 0xE4E4F2);
    text(x + 8, y + h - 19, u"Enter開く BS戻る Delゴミ箱 R復元", 0x55556A);
}
static void files_click(int x, int y) {
    (void)x;
    int i = ftop + (y - 28) / 20;
    if (y < 26) { Key k = {0, 8}; files_key(k); }
    else if (y >= 28 && i < fcount) { if (i == fsel) files_enter(); else fsel = i; }
}
static void files_tick(void) { static int n; if (++n % 50 == 0) { files_load(); invalidate(); } }

/* ==================== メモ帳 ==================== */
#define EMAX (30 * 1024)
static c16 ebuf[EMAX], epath[PATHLEN], romaji[8];
static int elen, ecur, etop, edirty, eidle, kana, nroma, efailed;

static void edit_save(void) {
    static u8 out[EMAX * 3];
    if (!edirty || !epath[0]) return;
    if (fs_write(epath, out, utf8_encode(ebuf, elen, out))) edirty = efailed = 0;
    else if (!efailed) { efailed = 1; notify(u"保存できません"); }
    invalidate();
}
static void insert(c16 c) {
    if (elen >= EMAX) { notify(u"これ以上は書けません"); return; }
    mcopy(ebuf + ecur + 1, ebuf + ecur, (elen - ecur) * 2);
    ebuf[ecur++] = c; elen++; edirty = 1; eidle = 0;
}
static void erase(int at) {
    if (at < 0 || at >= elen) return;
    mcopy(ebuf + at, ebuf + at + 1, (elen - at - 1) * 2);
    elen--; edirty = 1; eidle = 0;
}
static int new_file(void) {
    for (int i = 1; i < 1000; i++) {
        c16 p[PATHLEN] = u"\\MEMO";
        snum(p, i, 0, PATHLEN); scat(p, u".TXT", PATHLEN);
        if (!fs_stat(p, 0)) { scopy(epath, p, PATHLEN); elen = ecur = etop = edirty = efailed = 0; return 1; }
    }
    return 0;
}
void edit_open(const c16 *path) {
    edit_save();
    if (edirty) { notify(u"今のメモが保存できないため、開けません"); wm_open(APP_EDIT); return; }
    static c16 tmp[EMAX];
    int n = 0;
    if (fs_stat(path, 0)) {
        i64 bytes = fs_read(path, iobuf, sizeof iobuf);
        n = bytes < 0 ? -1 : utf8_decode(iobuf, bytes, tmp, EMAX);
        /* 読めないファイルは開かない (上書きして壊さないため) */
        if (n < 0) { notify(u"このファイルは開けません（大きすぎるか、テキストではない）"); return; }
    }
    int m = 0;
    for (int i = 0; i < n; i++) if (tmp[i] != u'\r' && tmp[i] != 0xFEFF) ebuf[m++] = tmp[i];
    scopy(epath, path, PATHLEN);
    elen = m; ecur = etop = edirty = efailed = nroma = 0;
    wm_open(APP_EDIT);
}
static void edit_start(void) { if (!epath[0] && fs_ready()) new_file(); }

/* ローマ字 → ひらがな */
static const struct { const char *r; const c16 *k; } table[] = {
#define R(a, b) {a, u##b},
#define ROW(c, a, i, u, e, o) R(c "a", a) R(c "i", i) R(c "u", u) R(c "e", e) R(c "o", o)
    ROW("", "あ", "い", "う", "え", "お") ROW("k", "か", "き", "く", "け", "こ") ROW("s", "さ", "し", "す", "せ", "そ")
    ROW("t", "た", "ち", "つ", "て", "と") ROW("n", "な", "に", "ぬ", "ね", "の") ROW("h", "は", "ひ", "ふ", "へ", "ほ")
    ROW("m", "ま", "み", "む", "め", "も") ROW("r", "ら", "り", "る", "れ", "ろ") ROW("g", "が", "ぎ", "ぐ", "げ", "ご")
    ROW("z", "ざ", "じ", "ず", "ぜ", "ぞ") ROW("d", "だ", "ぢ", "づ", "で", "ど") ROW("b", "ば", "び", "ぶ", "べ", "ぼ")
    ROW("p", "ぱ", "ぴ", "ぷ", "ぺ", "ぽ")
    ROW("ky", "きゃ", "きぃ", "きゅ", "きぇ", "きょ") ROW("sh", "しゃ", "し", "しゅ", "しぇ", "しょ") ROW("sy", "しゃ", "しぃ", "しゅ", "しぇ", "しょ")
    ROW("ch", "ちゃ", "ち", "ちゅ", "ちぇ", "ちょ") ROW("ty", "ちゃ", "ちぃ", "ちゅ", "ちぇ", "ちょ") ROW("ny", "にゃ", "にぃ", "にゅ", "にぇ", "にょ")
    ROW("hy", "ひゃ", "ひぃ", "ひゅ", "ひぇ", "ひょ") ROW("my", "みゃ", "みぃ", "みゅ", "みぇ", "みょ") ROW("ry", "りゃ", "りぃ", "りゅ", "りぇ", "りょ")
    ROW("gy", "ぎゃ", "ぎぃ", "ぎゅ", "ぎぇ", "ぎょ") ROW("j", "じゃ", "じ", "じゅ", "じぇ", "じょ") ROW("by", "びゃ", "びぃ", "びゅ", "びぇ", "びょ")
    ROW("py", "ぴゃ", "ぴぃ", "ぴゅ", "ぴぇ", "ぴょ") ROW("x", "ぁ", "ぃ", "ぅ", "ぇ", "ぉ")
    R("ya", "や") R("yu", "ゆ") R("yo", "よ") R("wa", "わ") R("wo", "を") R("tsu", "つ") R("fu", "ふ")
    R("fa", "ふぁ") R("fi", "ふぃ") R("fe", "ふぇ") R("fo", "ふぉ") R("xtu", "っ") R("xya", "ゃ") R("xyu", "ゅ") R("xyo", "ょ")
    R("-", "ー") R(",", "、") R(".", "。")
};
#define NTABLE (int)(sizeof table / sizeof *table)
static void emit(const c16 *s) { while (*s) insert(*s++); }
static void flush_romaji(void) {
    if ((nroma == 1 || (nroma == 2 && romaji[1] == u'n')) && romaji[0] == u'n') insert(u'ん');
    else for (int i = 0; i < nroma; i++) insert(romaji[i]);
    nroma = 0;
}
static void kana_input(c16 c) {
    if (c >= u'A' && c <= u'Z') c += 32;
    /* "nn" は次の文字を見て決める: konnichiha→こんにちは, honn→ほん */
    if (nroma == 2 && romaji[0] == u'n' && romaji[1] == u'n') {
        insert(u'ん');
        if (c == u'a' || c == u'i' || c == u'u' || c == u'e' || c == u'o') nroma = 1;
        else nroma = 0;
    }
    romaji[nroma++] = c;
    if (nroma == 2 && romaji[0] == u'n' && c == u'n') return;
    while (nroma) {
        int prefix = 0;
        for (int t = 0; t < NTABLE; t++) {
            int i = 0;
            while (i < nroma && table[t].r[i] == romaji[i]) i++;
            if (i < nroma) continue;
            if (!table[t].r[i]) { emit(table[t].k); nroma = 0; return; }
            prefix = 1;
        }
        if (prefix) return;
        if (nroma >= 2 && romaji[0] == u'n') insert(u'ん');
        else if (nroma >= 2 && romaji[0] == romaji[1] && romaji[0] >= u'a' && romaji[0] <= u'z') insert(u'っ');
        else insert(romaji[0]);
        mcopy(romaji, romaji + 1, (--nroma) * 2);
    }
}

static int line_start(int p) { while (p > 0 && ebuf[p - 1] != u'\n') p--; return p; }
static int line_end(int p) { while (p < elen && ebuf[p] != u'\n') p++; return p; }
static void edit_key(Key k) {
    if (!epath[0]) return;
    if (k.scan == K_F3) { flush_romaji(); kana ^= 1; return; }
    if (k.ch >= 32) { if (kana && k.ch < 127 && k.ch != u' ') kana_input(k.ch); else { flush_romaji(); insert(k.ch); } return; }
    if (k.ch == 8) { if (nroma) nroma--; else if (ecur) erase(--ecur); return; }
    flush_romaji();
    int s = line_start(ecur), col = ecur - s;
    if (k.ch == 13) insert(u'\n');
    else if (k.ch == 9) insert(u' '), insert(u' ');
    else if (k.scan == K_F2) { edirty = 1; edit_save(); if (!edirty) notify(u"保存しました"); }
    else if (k.scan == K_INS) { edit_save(); if (!edirty) new_file(); }
    else if (k.scan == K_DEL) erase(ecur);
    else if (k.scan == K_LEFT && ecur) ecur--;
    else if (k.scan == K_RIGHT && ecur < elen) ecur++;
    else if (k.scan == K_HOME) ecur = s;
    else if (k.scan == K_END) ecur = line_end(ecur);
    else if (k.scan == K_UP && s) { int p = line_start(s - 1); ecur = p + col > s - 1 ? s - 1 : p + col; }
    else if (k.scan == K_DOWN && line_end(ecur) < elen) { int p = line_end(ecur) + 1, e = line_end(p); ecur = p + col > e ? e : p + col; }
}
/* 画面の折り返しを考慮して描く。1回目でカーソル行を調べ、2回目で描画する */
static void edit_draw(int x, int y, int w, int h) {
    fill(x, y, w, h, 0xFFFFFF);
    int rows = (h - 30) / 18, crow = 0;
    for (int pass = 0; pass < 2; pass++) {
        int row = 0, px = 0;
        for (int i = 0; i <= elen; i++) {
            c16 c = i < elen ? ebuf[i] : 0;
            int cw = c && c != u'\n' ? charw(c) : 8;
            if (px + cw > w - 20 && c != u'\n') row++, px = 0;
            int vis = row >= etop && row < etop + rows, py = y + 4 + (row - etop) * 18;
            if (i == ecur) {
                crow = row;
                if (pass && vis) {
                    fill(x + 8 + px, py, 2, 16, 0x6D5DFC);
                    if (nroma) { c16 r[8]; mcopy(r, romaji, nroma * 2); r[nroma] = 0; fill(x + 10 + px, py, nroma * 8, 16, 0xFFE9A8); text(x + 10 + px, py, r, 0x6B4E00); }
                }
            }
            if (c == u'\n') { row++; px = 0; continue; }
            if (pass && vis && c && !(nroma && i >= ecur && row == crow)) glyph(x + 8 + px + (i == ecur ? 2 : 0), py, c, 0x1D1D27);
            px += cw;
        }
        if (!pass) { if (crow < etop) etop = crow; if (crow >= etop + rows) etop = crow - rows + 1; }
    }
    fill(x, y + h - 22, w, 22, 0xE4E4F2);
    c16 s[PATHLEN + 64] = u"";
    scat(s, kana ? u"[あ] " : u"[A] ", PATHLEN + 64);
    scat(s, epath[0] ? epath + 1 : u"保存先なし", PATHLEN + 64);
    scat(s, edirty ? u" *未保存" : u"", PATHLEN + 64);
    scat(s, u"   F2保存 F3かな Ins新規", PATHLEN + 64);
    text(x + 8, y + h - 19, s, 0x55556A);
}
static void edit_tick(void) { if (edirty && ++eidle == 100) edit_save(), eidle = 0; } /* 約2秒で自動保存 */
static void edit_flush(void) { flush_romaji(); edit_save(); }

/* ==================== 情報 ==================== */
#if defined(__aarch64__)
#define ARCH u"ARM64"
static void cpu_name(c16 *out) {
    u64 id;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(id));
    u32 maker = (id >> 24) & 255;
    scopy(out, maker == 0x61 ? u"Apple" : maker == 0x41 ? u"Arm" : maker == 0x51 ? u"Qualcomm" : u"ARM64 互換", 64);
    scat(out, u" (part ", 64); snum(out, (id >> 4) & 0xFFF, 0, 64); scat(out, u")", 64);
}
#else
#define ARCH u"x86-64"
static void cpu_name(c16 *out) {
    u32 r[13] = {0}, a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000), "c"(0));
    if (a >= 0x80000004) for (int i = 0; i < 3; i++)
        __asm__ volatile("cpuid" : "=a"(r[i * 4]), "=b"(r[i * 4 + 1]), "=c"(r[i * 4 + 2]), "=d"(r[i * 4 + 3]) : "a"(0x80000002 + i), "c"(0));
    const char *p = (const char *)r;
    while (*p == ' ') p++;
    int i = 0;
    for (; p[i] && i < 48; i++) out[i] = (u8)p[i];
    out[i] = 0;
}
#endif
static c16 cpu[64];
static u64 megabytes;
static void about_open(void) { cpu_name(cpu); megabytes = total_memory(); }
static void about_draw(int x, int y, int w, int h) {
    (void)w; (void)h;
    c16 s[96] = u"メモリ: ";
    text(x + 16, y + 14, u"FreeOS Any 1.3  (" ARCH u" / UEFI)", 0x1D1D27);
    text(x + 16, y + 44, u"CPU:", 0x55556A);
    text(x + 56, y + 44, cpu, 0x1D1D27);
    snum(s, megabytes, 0, 96); scat(s, u" MB", 96);
    text(x + 16, y + 66, s, 0x1D1D27);
    s[0] = 0; scat(s, u"画面: ", 96); snum(s, SW, 0, 96); scat(s, u" x ", 96); snum(s, SH, 0, 96);
    text(x + 16, y + 88, s, 0x1D1D27);
    text(x + 16, y + 122, u"広告 0 / 勝手な更新 0 / 追跡 0", 0x5A46E0);
    text(x + 16, y + 150, u"内蔵ディスクには書き込みません。", 0x55556A);
    text(x + 16, y + 170, u"通信は、ブラウザ・get・ファイル転送を使う時だけ。", 0x55556A);
}

App apps[APP_COUNT] = {
    [APP_TERM]  = {u"ターミナル", 600, 380, term_open, term_draw, term_key, 0, 0, 0},
    [APP_FILES] = {u"ファイル", 420, 360, files_load, files_draw, files_key, files_click, files_tick, 0},
    [APP_EDIT]  = {u"エディター", 580, 400, edit_start, edit_draw, edit_key, 0, edit_tick, edit_flush},
    [APP_BROWSER]   = {u"ブラウザ", 720, 520, browser_open, browser_draw, browser_key, browser_click, 0, 0},
    [APP_TRANSFER]  = {u"ファイル転送", 440, 250, 0, transfer_draw, transfer_key, transfer_click, transfer_tick, transfer_stop},
    [APP_SOLITAIRE] = {u"ソリティア", 394, 480, solitaire_open, solitaire_draw, solitaire_key, solitaire_click, 0, 0},
    [APP_ABOUT] = {u"情報", 440, 220, about_open, about_draw, 0, 0, 0, 0},
};
