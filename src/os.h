#pragma once
#include "efi.h"

extern SystemTable *ST;
extern BootServices *BS;

/* ---- 文字列 ---- */
int slen(const c16 *s);
int seq(const c16 *a, const c16 *b);
void scopy(c16 *d, const c16 *s, int cap);
void scat(c16 *d, const c16 *s, int cap);
void snum(c16 *d, i64 v, int width, int cap); /* 末尾に追記。widthまで0埋め */
void mcopy(void *d, const void *s, u64 n);    /* 重なっていてもよい */

/* ---- 描画 ---- */
extern int SW, SH;
void clip(int x, int y, int w, int h);
void noclip(void);
void fill(int x, int y, int w, int h, u32 color);
int glyph(int x, int y, c16 ch, u32 color);       /* 戻り値は文字幅 */
int text(int x, int y, const c16 *s, u32 color);  /* 戻り値は右端 */
int charw(c16 ch);

/* ---- ファイル (起動ドライブの \HOME の中だけを扱う) ---- */
#define PATHLEN 200
#define MAXLIST 128
typedef struct { c16 name[64]; u64 size; int dir; } Entry;
int fs_ready(void);
void fs_init(Handle image);
int fs_resolve(const c16 *cwd, const c16 *arg, c16 *out); /* out は "" か "\a\b" */
int fs_stat(const c16 *path, Entry *e);
int fs_list(const c16 *path, Entry *out, int max);
i64 fs_read(const c16 *path, u8 *buf, u64 cap);           /* 失敗や cap 超えは -1 */
int fs_write(const c16 *path, const u8 *buf, u64 len);
int fs_mkdir(const c16 *path);
int fs_move(const c16 *from, const c16 *to);
int fs_delete(const c16 *path);
int fs_trash(const c16 *path);
int utf8_decode(const u8 *in, u64 n, c16 *out, int cap);  /* 文字として読めなければ -1 */
int utf8_encode(const c16 *in, int n, u8 *out);

/* ---- ウィンドウ ---- */
enum { APP_TERM, APP_FILES, APP_EDIT, APP_BROWSER, APP_TRANSFER, APP_SOLITAIRE, APP_CALC, APP_AI, APP_ABOUT, APP_COUNT };
void wm_open(int app);
void wm_close(int app);
int wm_active(int app); /* 一番手前か */
void notify(const c16 *msg);
void invalidate(void);
void redraw(void); /* 時間のかかる処理の前にすぐ描き直す */
#ifndef HOST_LIBC /* Mac アプリ版の host_*.c は macOS の string.h を使う */
void *memset(void *d, int c, u64 n);
#endif
void power(int reboot);
void ui_busy(int app);    /* 通信中のアプリ (-1 で終わり)。その間も OS は動き、Esc で中止 */
int ui_is_busy(int app);

typedef struct {
    const c16 *title;
    int w, h;
    void (*open)(void);
    void (*draw)(int x, int y, int w, int h);
    void (*key)(Key k);
    void (*click)(int x, int y); /* ウィンドウ内の座標 */
    void (*tick)(void);          /* 約20msごと */
    void (*flush)(void);         /* 閉じる前・電源を切る前の保存 */
    void (*wheel)(int dy);       /* ホイール (下へ +)。なければ矢印キーとして送る */
} App;
extern App apps[APP_COUNT];
void edit_open(const c16 *path);
void kana_feed(c16 c, void (*out)(c16)); void kana_flush(void (*out)(c16)); int kana_pending(c16 *out); /* ローマ字かな入力 */

/* ---- 起動アニメーションと起動音 ---- */
#define FREEOS_VER u"2.5"
u32 *screen(void);                     /* 描画先 (SW x SH) */
void screen_show(void);                /* 描画先を画面に出す */
void desktop_compose(void);            /* デスクトップを描画先に描く (画面には出さない) */
int glyph_bits(c16 ch, const u16 **bits); /* 文字の点の並び (16行) と幅 */
const u8 *title_bitmap(c16 ch, int *w, int *adv, int *h); /* 起動画面の名前用の大きな文字 */
void boot_splash(void);
void sound_init(void);
void sound_chime(void);
void sound_poll(void);

/* ---- マウス ---- */
void tablet_init(void);
void tablet_poll(void);
void pointer_abs(int x, int y, int down); /* 座標は 0〜32767 */
void pointer_wheel(int dy);               /* 下へ + */

/* 別ファイルのアプリ */
void browser_open(void); void browser_draw(int x, int y, int w, int h); void browser_key(Key k); void browser_click(int x, int y);
void browser_go(const char *url); void browser_tick(void); void browser_wheel(int dy); void browser_open_file(const c16 *path);
int download(const char *url, c16 *path);
void transfer_draw(int x, int y, int w, int h); void transfer_key(Key k); void transfer_click(int x, int y); void transfer_tick(void); void transfer_stop(void);
void chat_ask(const c16 *q); void chat_open(void); void chat_draw(int x, int y, int w, int h); void chat_key(Key k); void chat_click(int x, int y); void chat_tick(void); void chat_wheel(int dy);
void calc_open(void); void calc_draw(int x, int y, int w, int h); void calc_key(Key k); void calc_click(int x, int y); void calc_tick(void);
void solitaire_open(void); void solitaire_draw(int x, int y, int w, int h); void solitaire_key(Key k); void solitaire_click(int x, int y);
