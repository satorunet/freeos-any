/* ネットワーク: 有線LAN (UEFI Simple Network Protocol) の上に自作の TCP/IP */
#pragma once
#include "os.h"

u64 net_ms(void);
void net_init(Handle image);
int net_up(void);                 /* 初期化して IP を取得。失敗は 0 */
int net_ready(void);
const c16 *net_error(void);
u32 net_ip(void);
void net_ipstr(u32 ip, c16 *out, int cap);
void net_poll(void);
int net_wait(void);               /* 少し待って受信処理。Esc が押されたら 0 */
int net_resolve(const char *host, u32 *ip);

int tcp_connect(u32 ip, u16 port);            /* 失敗は -1 */
int tcp_listen(u16 port);
void tcp_unlisten(void);
int tcp_accept(void);                         /* 新しい接続がなければ -1 */
int tcp_send(int s, const void *buf, int len); /* 受け付けたバイト数。切断は -1 */
int tcp_recv(int s, void *buf, int cap);      /* 0=まだ無い, -1=相手が閉じた, -2=異常切断 */
int tcp_sent_all(int s);
void tcp_close(int s);
void tcp_abort(int s);

int entropy(u8 *out, int n);      /* 暗号用の乱数。用意できなければ 0 */

/* HTTP / HTTPS */
#define URLLEN 512
typedef struct { int status, truncated; char type[80]; char url[URLLEN]; u64 len; } HttpResult;
int http_get(const char *url, u8 *buf, u64 cap, HttpResult *r); /* 失敗は 0 (net_error 参照) */
void net_set_error(const c16 *msg);
