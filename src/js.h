/* 小さな JavaScript 実行エンジン (ブラウザ用)。文字列は UTF-16。
 * メモリはページごとの領域から取り、ページを移るときにまとめて捨てる (js_reset)。 */
#pragma once
#include "os.h"

enum { JS_UNDEF, JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_OBJ };
typedef struct JsStr { int len; c16 s[]; } JsStr;
typedef struct JsObj JsObj;
typedef struct { u8 t; double n; void *p; } JsVal;
typedef JsVal (*JsNative)(JsVal self, JsVal *args, int n);

/* ページ用の領域 (mem, size) で初期化する。グローバルも作り直す */
void js_reset(void *mem, u64 size);
/* スクリプトを実行する。エラーなら 0 (js_error() で内容) */
int js_run(const c16 *src, int len);
/* 関数を呼ぶ (イベント・タイマー用) */
int js_call(JsVal fn, JsVal self, JsVal *args, int n);
const c16 *js_error(void);

/* 値を作る・変える */
JsVal js_undef(void);
JsVal js_num(double v);
JsVal js_bool(int v);
JsVal js_str(const c16 *s, int len);
JsVal js_cstr(const char *s);
JsVal js_native(JsNative fn);
JsVal js_host(void *node);          /* ブラウザの要素を包む (同じ要素には同じ物を返す) */
JsVal js_object(void);
JsVal js_make_array(JsVal *items, int n);
JsVal js_global_get(const char *name);
void js_set(JsVal obj, const char *key, JsVal v);
void js_global(const char *name, JsVal v);
int js_truthy(JsVal v);
JsStr *js_tostr(JsVal v);
double js_tonum(JsVal v);
int js_is_func(JsVal v);
void *js_host_node(JsVal v);        /* 要素でなければ 0 */
int js_key_is(const JsStr *k, const char *s);

/* ブラウザ側が用意する: 要素のプロパティの読み書き */
JsVal host_get(void *node, const JsStr *key, int *found);
int host_set(void *node, const JsStr *key, JsVal v);
void *host_jsobj_slot(void *node, JsObj ***slot); /* 要素ごとの包み物の置き場 */
