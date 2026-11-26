/* 仮想マシン用のキーボード (virtio-keyboard)。「かな」「英数」キーも受け取れる */
#pragma once
#include "os.h"
#define K_KANA 0x100  /* 「かな」キー (Key.scan) */
#define K_EISU 0x101  /* 「英数」キー */
int vkbd_init(void);                 /* 見つかれば 1 */
int vkbd_read(Key *k, int *special); /* キーがあれば 1。special: 1 =「かな」 2 =「英数」 (そのとき k は使わない) */
