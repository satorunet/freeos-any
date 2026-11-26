/* Mac アプリ版: UEFI ファームウェアの代わり。
 * OS 本体 (src の C ファイル) はそのまま、ここが用意する SystemTable の上で動く。
 * 画面・キー・マウスは host_mac.m (Cocoa)、ファイルは Mac のフォルダ、通信は macOS の HTTPS を使う。 */
#define _DARWIN_C_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include "../src/efi.h"
#include "host.h"

#define OK 0
#define NOT_FOUND 0x800000000000000EULL
#define DEV_ERR 0x8000000000000007ULL
#define BUF_SMALL 0x8000000000000005ULL

static int guid_is(const Guid *g, u32 a) { return g->a == a; }
#define G_LOADED 0x5B1B31A1
#define G_FS 0x964e5b22
#define G_FILEINFO 0x09576e92
#define G_GOP 0x9042a9de
#define G_REL 0x31878c87
#define G_ABS 0x8D59D32B
#define G_RNG 0x3152bca5

/* ---- 入力の受け渡し (Cocoa のスレッド → OS のスレッド) ---- */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Key keys[256]; static int kHead, kTail;
static int mouseX, mouseY, mouseBtn, wheelZ;
void host_key(int scan, int ch) {
    pthread_mutex_lock(&lock);
    int n = (kTail + 1) % 256;
    if (n != kHead) { keys[kTail].scan = (u16)scan; keys[kTail].ch = (c16)ch; kTail = n; }
    pthread_mutex_unlock(&lock);
}
void host_mouse(int x, int y, int buttons) { pthread_mutex_lock(&lock); mouseX = x; mouseY = y; mouseBtn = buttons; pthread_mutex_unlock(&lock); }
void host_wheel(int dz) { pthread_mutex_lock(&lock); wheelZ += dz; pthread_mutex_unlock(&lock); }

static Status read_key(TextIn *t, Key *k) {
    (void)t; Status s = NOT_FOUND;
    pthread_mutex_lock(&lock);
    if (kHead != kTail) { *k = keys[kHead]; kHead = (kHead + 1) % 256; s = OK; }
    pthread_mutex_unlock(&lock);
    return s;
}
static Status noop_reset_in(TextIn *t, u8 x) { (void)t; (void)x; return OK; }
static TextIn conIn = {noop_reset_in, read_key, 0};

/* マウス: 位置は絶対座標、ホイールだけ相対 (z) */
static AbsMode absMode = {0, 0, 0, FB_W - 1, FB_H - 1, 0, 0};
static Status abs_reset(AbsPointer *p, u8 x) { (void)p; (void)x; return OK; }
static Status abs_state(AbsPointer *p, AbsState *s) {
    (void)p; static int lx = -1, ly, lb = -1;
    pthread_mutex_lock(&lock); int x = mouseX, y = mouseY, b = mouseBtn; pthread_mutex_unlock(&lock);
    if (x == lx && y == ly && b == lb) return NOT_FOUND; /* 変わっていない */
    lx = x; ly = y; lb = b;
    s->x = (u64)x; s->y = (u64)y; s->z = 0; s->buttons = (u32)b;
    return OK;
}
static AbsPointer absPtr = {abs_reset, abs_state, 0, &absMode};
static PointerMode relMode = {1, 1, 1, 1, 1};
static Status rel_reset(Pointer *p, u8 x) { (void)p; (void)x; return OK; }
static Status rel_state(Pointer *p, PointerState *s) {
    (void)p;
    pthread_mutex_lock(&lock); int z = wheelZ; wheelZ = 0; pthread_mutex_unlock(&lock);
    if (!z) return NOT_FOUND;
    pthread_mutex_lock(&lock); int b = mouseBtn; pthread_mutex_unlock(&lock);
    s->x = s->y = 0; s->z = z; s->left = (u8)(b & 1); s->right = 0; /* 上へ回すと z が + (OS は z > 0 で上へ)。ボタンは今の状態のまま */
    return OK;
}
static Pointer relPtr = {rel_reset, rel_state, 0, &relMode};

/* ---- 画面 ---- */
static u32 fb[FB_W * FB_H];
static GopInfo gopInfo = {0, FB_W, FB_H, 1, {0}, FB_W};
static GopMode gopMode = {1, 0, &gopInfo, sizeof(GopInfo), 0, sizeof fb};
static Status gop_query(Gop *g, u32 m, u64 *sz, GopInfo **i) { (void)g; (void)m; *sz = sizeof gopInfo; *i = &gopInfo; return OK; }
static Status gop_set(Gop *g, u32 m) { (void)g; (void)m; return OK; }
static Status gop_blt(Gop *g, u32 *buf, u32 op, u64 sx, u64 sy, u64 dx, u64 dy, u64 w, u64 h, u64 delta) {
    (void)g; (void)sx; (void)sy;
    if (op != 2) return OK; /* BltBufferToVideo だけ使う */
    u64 stride = delta ? delta / 4 : w;
    for (u64 y = 0; y < h && dy + y < FB_H; y++) memcpy(&fb[(dy + y) * FB_W + dx], &buf[y * stride], (size_t)(w > FB_W - dx ? FB_W - dx : w) * 4);
    host_present(fb, FB_W, FB_H);
    return OK;
}
static Gop gop = {gop_query, gop_set, gop_blt, &gopMode};

/* ---- 乱数 ---- */
struct Rng { void *info; Status (*GetRNG)(struct Rng *, void *, u64, u8 *); };
static Status get_rng(struct Rng *r, void *alg, u64 n, u8 *out) { (void)r; (void)alg; arc4random_buf(out, (size_t)n); return OK; }
static struct Rng rng = {0, get_rng};

/* ---- ファイル (Mac のフォルダ。\ が / に) ---- */
typedef struct { File f; int fd; DIR *dir; char path[1100]; } HFile;
static char root[1024];
static File *make_file(const char *path, int fd, DIR *dir);
static void to_utf8(const c16 *s, char *out, int cap) {
    int k = 0;
    for (; *s && k < cap - 4; s++) {
        c16 c = *s == '\\' ? '/' : *s;
        if (c < 0x80) out[k++] = (char)c;
        else if (c < 0x800) out[k++] = (char)(0xC0 | c >> 6), out[k++] = (char)(0x80 | (c & 63));
        else out[k++] = (char)(0xE0 | c >> 12), out[k++] = (char)(0x80 | ((c >> 6) & 63)), out[k++] = (char)(0x80 | (c & 63));
    }
    out[k] = 0;
}
static int from_utf8(const char *s, c16 *out, int cap) {
    int k = 0; const unsigned char *p = (const unsigned char *)s;
    while (*p && k < cap - 1) {
        unsigned c = *p++;
        if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (c >= 0xC0) { c = (c & 31) << 6 | (p[0] & 63); p++; }
        out[k++] = (c16)c;
    }
    out[k] = 0; return k;
}
static void resolve(HFile *base, const c16 *name, char *out) {
    char rel[1024]; to_utf8(name, rel, sizeof rel);
    if (rel[0] == '/') snprintf(out, 1100, "%s%s", root, rel);
    else snprintf(out, 1100, "%s/%s", base->path, rel);
    /* "." と ".." を整理 (root より上には出ない) */
    size_t rl = strlen(root);
    char *p = out + rl, tmp[1100]; size_t k = 0;
    for (char *seg = strtok(p, "/"); seg; seg = strtok(0, "/")) {
        if (!strcmp(seg, ".") || !*seg) continue;
        if (!strcmp(seg, "..")) { while (k && tmp[k - 1] != '/') k--; if (k) k--; continue; }
        tmp[k++] = '/'; size_t l = strlen(seg); memcpy(tmp + k, seg, l); k += l;
    }
    tmp[k] = 0;
    snprintf(out, 1100, "%s%s", root, tmp);
}
static Status f_open(File *self, File **nf, c16 *name, u64 mode, u64 attr) {
    char path[1100]; resolve((HFile *)self, name, path);
    struct stat st;
    if (stat(path, &st)) {
        if (!(mode & FILE_CREATE)) return NOT_FOUND;
        if (attr & FILE_DIRECTORY) { if (mkdir(path, 0755)) return DEV_ERR; }
        else { int fd = open(path, O_RDWR | O_CREAT, 0644); if (fd < 0) return DEV_ERR; *nf = make_file(path, fd, 0); return OK; }
        stat(path, &st);
    }
    if (S_ISDIR(st.st_mode)) { DIR *d = opendir(path); if (!d) return DEV_ERR; *nf = make_file(path, -1, d); return OK; }
    int fd = open(path, (mode & FILE_WRITE) ? O_RDWR : O_RDONLY);
    if (fd < 0) return DEV_ERR;
    *nf = make_file(path, fd, 0);
    return OK;
}
static Status f_close(File *self) { HFile *h = (HFile *)self; if (h->fd >= 0) close(h->fd); if (h->dir) closedir(h->dir); free(h); return OK; }
static Status f_delete(File *self) { HFile *h = (HFile *)self; int r = h->dir ? rmdir(h->path) : unlink(h->path); f_close(self); return r ? DEV_ERR : OK; }
static void fill_info(const char *path, const char *name, FileInfo *fi) {
    struct stat st; memset(fi, 0, sizeof *fi);
    if (!stat(path, &st)) { fi->fileSize = (u64)st.st_size; fi->physicalSize = (u64)st.st_size; fi->attr = S_ISDIR(st.st_mode) ? FILE_DIRECTORY : 0; }
    int n = from_utf8(name, fi->name, 256);
    fi->size = 80 + (u64)(n + 1) * 2;
}
static Status f_read(File *self, u64 *n, void *buf) {
    HFile *h = (HFile *)self;
    if (h->dir) { /* フォルダ: 次の項目を FileInfo で */
        struct dirent *e;
        do e = readdir(h->dir); while (e && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || e->d_name[0] == '.'));
        if (!e) { *n = 0; return OK; }
        char p[1100]; snprintf(p, sizeof p, "%s/%s", h->path, e->d_name);
        FileInfo fi; fill_info(p, e->d_name, &fi);
        if (*n < fi.size) { *n = fi.size; return BUF_SMALL; }
        memcpy(buf, &fi, (size_t)fi.size); *n = fi.size; return OK;
    }
    ssize_t r = read(h->fd, buf, (size_t)*n);
    if (r < 0) return DEV_ERR;
    *n = (u64)r; return OK;
}
static Status f_write(File *self, u64 *n, void *buf) { HFile *h = (HFile *)self; ssize_t r = write(h->fd, buf, (size_t)*n); if (r < 0) return DEV_ERR; *n = (u64)r; return OK; }
static Status f_getpos(File *self, u64 *p) { HFile *h = (HFile *)self; *p = h->fd >= 0 ? (u64)lseek(h->fd, 0, SEEK_CUR) : 0; return OK; }
static Status f_setpos(File *self, u64 p) { HFile *h = (HFile *)self; if (h->dir) { rewinddir(h->dir); return OK; } if (p == ~0ull) lseek(h->fd, 0, SEEK_END); else lseek(h->fd, (off_t)p, SEEK_SET); return OK; }
static Status f_getinfo(File *self, Guid *g, u64 *n, void *buf) {
    HFile *h = (HFile *)self; (void)g;
    const char *name = strrchr(h->path, '/'); name = name ? name + 1 : h->path;
    FileInfo fi; fill_info(h->path, name, &fi);
    if (*n < fi.size) { *n = fi.size; return BUF_SMALL; }
    memcpy(buf, &fi, (size_t)fi.size); *n = fi.size; return OK;
}
static Status f_setinfo(File *self, Guid *g, u64 n, void *buf) { /* 名前の変更 (移動) と大きさの変更 */
    HFile *h = (HFile *)self; (void)g; (void)n;
    FileInfo *fi = buf;
    char to[1100]; resolve(h, fi->name, to);
    if (strcmp(to, h->path)) { if (rename(h->path, to)) return DEV_ERR; snprintf(h->path, sizeof h->path, "%s", to); }
    if (h->fd >= 0 && !(fi->attr & FILE_DIRECTORY)) { struct stat st; if (!fstat(h->fd, &st) && (u64)st.st_size != fi->fileSize) if (ftruncate(h->fd, (off_t)fi->fileSize)) return DEV_ERR; }
    return OK;
}
static Status f_flush(File *self) { HFile *h = (HFile *)self; if (h->fd >= 0) fsync(h->fd); return OK; }
static File *make_file(const char *path, int fd, DIR *dir) {
    HFile *h = calloc(1, sizeof *h);
    h->f.rev = 0x10000; h->f.Open = f_open; h->f.Close = f_close; h->f.Delete = f_delete; h->f.Read = f_read; h->f.Write = f_write;
    h->f.GetPosition = f_getpos; h->f.SetPosition = f_setpos; h->f.GetInfo = f_getinfo; h->f.SetInfo = f_setinfo; h->f.Flush = f_flush;
    h->fd = fd; h->dir = dir; snprintf(h->path, sizeof h->path, "%s", path);
    return &h->f;
}
static Status open_volume(FileSystem *fs, File **f) { (void)fs; DIR *d = opendir(root); if (!d) return DEV_ERR; *f = make_file(root, -1, d); return OK; }
static FileSystem sfs = {0x10000, open_volume};
static int device;
static LoadedImage loaded = {0x1000, 0, 0, &device};

/* ---- ブートサービス ---- */
static Status allocate_pool(u32 type, u64 n, void **p) { (void)type; *p = malloc(n ? (size_t)n : 1); return *p ? OK : 0x8000000000000009ULL; }
static Status free_pool(void *p) { free(p); return OK; }
static u64 period100ns = 200000, nextTick;
static u64 now_100ns(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom / 100; }
static Status create_event(u32 t, u64 tpl, void *f, void *ctx, Event *e) { (void)t; (void)tpl; (void)f; (void)ctx; static int ev; *e = &ev; return OK; }
static Status set_timer(Event e, u32 type, u64 t) { (void)e; (void)type; period100ns = t ? t : 200000; nextTick = now_100ns() + period100ns; return OK; }
static Status wait_for_event(u64 n, Event *e, u64 *idx) {
    (void)n; (void)e; *idx = 0;
    u64 now = now_100ns();
    if (now < nextTick) usleep((useconds_t)((nextTick - now) / 10));
    nextTick += period100ns; if (nextTick < now_100ns()) nextTick = now_100ns() + period100ns;
    return OK;
}
static Status handle_protocol(Handle h, Guid *g, void **out) {
    if (guid_is(g, G_LOADED) && h == (Handle)&loaded) { *out = &loaded; return OK; }
    if (guid_is(g, G_FS) && h == (Handle)&device) { *out = &sfs; return OK; }
    if (guid_is(g, G_ABS) && h == (Handle)&absPtr) { *out = &absPtr; return OK; }
    if (guid_is(g, G_REL) && h == (Handle)&relPtr) { *out = &relPtr; return OK; }
    return NOT_FOUND;
}
static Status open_protocol(Handle h, Guid *g, void **out, Handle a, Handle c, u32 attr) { (void)a; (void)c; (void)attr; return handle_protocol(h, g, out); }
static Status locate_handle_buffer(u32 type, Guid *g, void *key, u64 *n, Handle **out) {
    (void)type; (void)key;
    Handle h = 0;
    if (guid_is(g, G_ABS)) h = &absPtr;
    else if (guid_is(g, G_REL)) h = &relPtr;
    if (!h) return NOT_FOUND; /* PCI の装置 (仮想マシン用) や LAN はない */
    *out = malloc(sizeof(Handle)); (*out)[0] = h; *n = 1;
    return OK;
}
static Status locate_protocol(Guid *g, void *reg, void **out) {
    (void)reg;
    if (guid_is(g, G_GOP)) { *out = &gop; return OK; }
    if (guid_is(g, G_RNG)) { *out = &rng; return OK; }
    return NOT_FOUND;
}
static Status stall(u64 us) { usleep((useconds_t)us); return OK; }
static Status watchdog(u64 a, u64 b, u64 c, c16 *d) { (void)a; (void)b; (void)c; (void)d; return OK; }
static Status memory_map(u64 *size, void *map, u64 *key, u64 *dsize, u32 *ver) {
    *dsize = sizeof(MemoryDesc); *ver = 1; *key = 0;
    if (*size < sizeof(MemoryDesc)) { *size = sizeof(MemoryDesc); return BUF_SMALL; }
    MemoryDesc *d = map; memset(d, 0, sizeof *d); d->type = 7; d->pages = host_memory_bytes() / 4096;
    *size = sizeof(MemoryDesc); return OK;
}
static BootServices bs;

/* ---- 実行時サービス ---- */
static Status get_time(Time *t, void *cap) {
    (void)cap; time_t now = time(0); struct tm tm; localtime_r(&now, &tm); /* Mac 版は Mac の時刻 (日本時間など) */
    memset(t, 0, sizeof *t);
    t->year = (u16)(tm.tm_year + 1900); t->month = (u8)(tm.tm_mon + 1); t->day = (u8)tm.tm_mday;
    t->hour = (u8)tm.tm_hour; t->minute = (u8)tm.tm_min; t->second = (u8)tm.tm_sec;
    return OK;
}
static void reset_system(u32 type, Status s, u64 n, void *d) { (void)s; (void)n; (void)d; host_quit(type == 0); }
static RuntimeServices rt;
static Status con_out(void *c, const c16 *s) { (void)c; char b[512]; to_utf8(s, b, sizeof b); fprintf(stderr, "%s", b); return OK; }
static struct { void *reset; Status (*out)(void *, const c16 *); } conOut = {0, con_out};
static SystemTable st;

Status efi_main(Handle image, SystemTable *st);
/* OS のスレッド */
static void *os_thread(void *arg) {
    (void)arg;
    bs.AllocatePool = allocate_pool; bs.FreePool = free_pool; bs.CreateEvent = create_event; bs.SetTimer = set_timer;
    bs.WaitForEvent = wait_for_event; bs.HandleProtocol = handle_protocol; bs.OpenProtocol = open_protocol;
    bs.LocateHandleBuffer = locate_handle_buffer; bs.LocateProtocol = locate_protocol; bs.Stall = stall;
    bs.SetWatchdogTimer = watchdog; bs.GetMemoryMap = memory_map;
    rt.GetTime = get_time; rt.ResetSystem = reset_system;
    st.conIn = &conIn; st.conOut = &conOut; st.rt = &rt; st.bs = &bs;
    efi_main((Handle)&loaded, &st);
    host_quit(0);
    return 0;
}
void host_start(const char *dataDir) {
    snprintf(root, sizeof root, "%s", dataDir);
    mkdir(root, 0755);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64 << 20); /* 大きな配列をスタックに置く関数がある */
    pthread_t th; pthread_create(&th, &a, os_thread, 0);
}
