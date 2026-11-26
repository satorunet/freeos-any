/* 正規表現 (JavaScript の書き方) */
#pragma once
#include "os.h"
void *memcpy(void *d, const void *s, u64 n);
typedef struct Regex Regex;
Regex *re_compile(const c16 *pat, int n, const char *flags, void *(*alloc)(u64)); /* 書き方が違えば 0 */
int re_ncap(const Regex *r);
int re_group_name(const Regex *r, int i, const c16 **name, int *idx);
int re_exec(Regex *re, const c16 *s, int len, int start, int sticky, int *caps); /* caps は (ncap+1)*2 個 */
