/* AI チャット用の小さな推論エンジン (Qwen3、tools/export_qwen3.py で作った .fqw を使う) */
#pragma once
#include "os.h"

#define LLM_CTX 512                  /* 覚えておける長さ (トークン) */
void *llm_alloc(u64 bytes);          /* OS / 試験用プログラムが用意する */
int llm_load(u8 *data, u64 len);     /* 失敗なら 0 */
const char *llm_error(void);
int llm_has_avx2(void);
/* 会話: 発言を渡して、返事を少しずつ受け取る */
int llm_begin(const char *utf8, int len);  /* 失敗 (長すぎる) なら 0 */
int llm_step(char *out, int cap);          /* 返事の続き (UTF-8) の長さ。終わりなら -1 */
void llm_reset(void);                       /* 会話を忘れる */
int llm_pos(void);                          /* 使った長さ */
