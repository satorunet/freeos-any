/* Mac アプリ版: host_efi.c / host_net.c (C) と host_mac.m (Cocoa) のあいだ */
#pragma once
#define FB_W 1280
#define FB_H 800
/* Cocoa → OS */
void host_start(const char *dataDir);
void host_key(int scan, int ch);
void host_mouse(int x, int y, int buttons);
void host_wheel(int dz);
/* OS → Cocoa */
void host_present(const unsigned int *pixels, int w, int h);
void host_quit(int reboot);
unsigned long long host_memory_bytes(void);
/* 通信 (macOS の URLSession)。始めて番号を返す → 終わるまで host_http_poll */
int host_http_start(const char *url, const char *cookie);
/* 0 まだ / 1 終わった / -1 失敗。終わったら status, type, finalUrl, setCookie (改行区切り), 本文 */
int host_http_poll(int id, int *status, char *type, int typeCap, char *finalUrl, int urlCap, char *setCookie, int scCap,
                   unsigned char *buf, unsigned long long cap, unsigned long long *len, int *truncated, char *err, int errCap);
void host_http_cancel(int id);
