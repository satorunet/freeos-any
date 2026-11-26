/* 小さな推論エンジン: Qwen3 (重み 4 ビット、埋め込み 8 ビット)。1 コアで動く。
 * 計算の流れと形式は llm.asm (https://github.com/alesha-pro/llm.asm, MIT) を参考にした。
 * x86-64 で AVX2 があれば 4 ビット×8 ビットの掛け算を速くする。なければ普通の計算。 */
#include "llm.h"
#if defined(__x86_64__)
#include <immintrin.h>
#define SIMD __attribute__((target("avx2,fma,f16c")))
#endif

typedef signed char i8;
typedef struct __attribute__((packed)) { u16 d; u8 q[16]; } BQ4;
typedef struct __attribute__((packed)) { u16 d; i8 q[32]; } BQ8;
typedef struct { float d; i8 q[32]; } XQ8; /* 計算途中の値を 8 ビットにしたもの */

static struct {
    int H, L, NH, NKV, HD, FF, V, nmerge; float eps, theta;
    const u32 *tokOff; const u8 *tokBlob; const u32 *merges;
    struct { const float *attnNorm, *qNorm, *kNorm, *postNorm; const void *q, *k, *v, *o, *gate, *up, *down; } *layer;
    int q8; /* 層の重みが 8 ビット */
    const float *finalNorm; const BQ8 *emb;
} M;
static const char *err = "";
const char *llm_error(void) { return err; }

/* ---- 数学 ---- */
static float h2f[65536];
static float f16(u16 h) {
    u32 s = (u32)(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 1023, b;
    if (e == 0) { if (!m) b = s; else { e = 1; while (!(m & 1024)) m <<= 1, e--; m &= 1023; b = s | ((e + 112) << 23) | (m << 13); } }
    else if (e == 31) b = s | 0x7F800000 | (m << 13);
    else b = s | ((e + 112) << 23) | (m << 13);
    float f; __builtin_memcpy(&f, &b, 4); return f;
}
static float fsqrt(float x) {
#if defined(__aarch64__)
    float r; __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x)); return r;
#else
    float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r;
#endif
}
static float fexp(float x) { /* e^x */
    if (x < -87) return 0;
    if (x > 88) x = 88;
    float t = x * 1.4426950409f; /* log2(e) */
    int i = (int)(t < 0 ? t - 1 : t); float f = t - i;      /* 2^t = 2^i * 2^f */
    float p = 1 + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * (0.0096181f + f * 0.0013334f))));
    u32 b = (u32)(i + 127) << 23; float s; __builtin_memcpy(&s, &b, 4);
    return p * s;
}
static double dexp(double x) { double t = 1, r = 1; int n = 0; while (x > 0.5) x -= 0.5, n++; while (x < -0.5) x += 0.5, n--; for (int k = 1; k < 25; k++) { t *= x / k; r += t; } for (; n > 0; n--) r *= 1.6487212707001282; for (; n < 0; n++) r /= 1.6487212707001282; return r; }
static double dsin(double x) { const double PI = 3.14159265358979323846; long k = (long)(x / (2 * PI)); x -= k * 2 * PI; if (x > PI) x -= 2 * PI; double x2 = x * x, t = x, r = x; for (int i = 1; i < 16; i++) { t *= -x2 / ((2 * i) * (2 * i + 1)); r += t; } return r; }
static float *ropeCos, *ropeSin; /* [LLM_CTX][HD/2] */

/* ---- 8 ビットにする (32 個ずつ) ---- */
static void quantize(const float *x, XQ8 *q, int n) {
    for (int b = 0; b < n / 32; b++) {
        float amax = 0;
        for (int i = 0; i < 32; i++) { float a = x[b * 32 + i] < 0 ? -x[b * 32 + i] : x[b * 32 + i]; if (a > amax) amax = a; }
        float d = amax / 127, id = d ? 1 / d : 0;
        q[b].d = d;
        for (int i = 0; i < 32; i++) { float v = x[b * 32 + i] * id; q[b].q[i] = (i8)(v < 0 ? v - 0.5f : v + 0.5f); }
    }
}
/* ---- 行列×ベクトル ---- */
static int avx2;
#if defined(__x86_64__)
SIMD static float hsum(__m256 v) { __m128 a = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1)); a = _mm_add_ps(a, _mm_movehl_ps(a, a)); a = _mm_add_ss(a, _mm_movehdup_ps(a)); return _mm_cvtss_f32(a); }
SIMD static __m256 dot32(__m256i qx, __m256i qy) {
    __m256i ax = _mm256_sign_epi8(qx, qx), sy = _mm256_sign_epi8(qy, qx);
    return _mm256_cvtepi32_ps(_mm256_madd_epi16(_mm256_maddubs_epi16(ax, sy), _mm256_set1_epi16(1)));
}
SIMD static void mv_q4_avx2(float *out, const BQ4 *w, const XQ8 *x, int rows, int cols) {
    int nb = cols / 32; const __m128i m4 = _mm_set1_epi8(15); const __m256i off = _mm256_set1_epi8(8);
    for (int r = 0; r < rows; r++) {
        const BQ4 *row = w + (u64)r * nb; __m256 acc = _mm256_setzero_ps();
        for (int b = 0; b < nb; b++) {
            __m128i t = _mm_loadu_si128((const __m128i *)row[b].q);
            __m256i qx = _mm256_sub_epi8(_mm256_set_m128i(_mm_and_si128(_mm_srli_epi16(t, 4), m4), _mm_and_si128(t, m4)), off);
            __m256i qy = _mm256_loadu_si256((const __m256i *)x[b].q);
            acc = _mm256_fmadd_ps(_mm256_set1_ps(h2f[row[b].d] * x[b].d), dot32(qx, qy), acc);
        }
        out[r] = hsum(acc);
    }
}
SIMD static void mv_q8_avx2(float *out, const BQ8 *w, const XQ8 *x, int rows, int cols) {
    int nb = cols / 32;
    for (int r = 0; r < rows; r++) {
        const BQ8 *row = w + (u64)r * nb; __m256 acc = _mm256_setzero_ps();
        for (int b = 0; b < nb; b++) {
            __m256i qx = _mm256_loadu_si256((const __m256i *)row[b].q), qy = _mm256_loadu_si256((const __m256i *)x[b].q);
            acc = _mm256_fmadd_ps(_mm256_set1_ps(h2f[row[b].d] * x[b].d), dot32(qx, qy), acc);
        }
        out[r] = hsum(acc);
    }
}
static int cpu_avx2(void) {
    u32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1 << 26)) || !(c & (1 << 28)) || !(c & (1 << 12)) || !(c & (1 << 29))) return 0; /* XSAVE AVX FMA F16C */
    if (!(c & (1 << 27))) { /* ファームウェアが XSAVE を有効にしていない: こちらで有効にする (UEFI の中は特権で動く) */
        u64 cr4; __asm__ volatile("mov %%cr4, %0" : "=r"(cr4)); __asm__ volatile("mov %0, %%cr4" :: "r"(cr4 | (1 << 18)));
    }
    u32 lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6) { lo |= 7; __asm__ volatile("xsetbv" :: "a"(lo), "d"(hi), "c"(0)); }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    return (b >> 5) & 1;
}
#else
static int cpu_avx2(void) { return 0; }
#endif
int llm_has_avx2(void) { return avx2; }
static void mv_q4(float *out, const BQ4 *w, const XQ8 *x, int rows, int cols) {
#if defined(__x86_64__)
    if (avx2) { mv_q4_avx2(out, w, x, rows, cols); return; }
#endif
    int nb = cols / 32;
    for (int r = 0; r < rows; r++) {
        const BQ4 *row = w + (u64)r * nb; float s = 0;
        for (int b = 0; b < nb; b++) {
            int sum = 0;
            for (int i = 0; i < 16; i++) sum += ((row[b].q[i] & 15) - 8) * x[b].q[i] + ((row[b].q[i] >> 4) - 8) * x[b].q[i + 16];
            s += h2f[row[b].d] * x[b].d * sum;
        }
        out[r] = s;
    }
}
static void mv_q8(float *out, const BQ8 *w, const XQ8 *x, int rows, int cols) {
#if defined(__x86_64__)
    if (avx2) { mv_q8_avx2(out, w, x, rows, cols); return; }
#endif
    int nb = cols / 32;
    for (int r = 0; r < rows; r++) {
        const BQ8 *row = w + (u64)r * nb; float s = 0;
        for (int b = 0; b < nb; b++) { int sum = 0; for (int i = 0; i < 32; i++) sum += row[b].q[i] * x[b].q[i]; s += h2f[row[b].d] * x[b].d * sum; }
        out[r] = s;
    }
}
static void mv(float *out, const void *w, const XQ8 *x, int rows, int cols) { if (M.q8) mv_q8(out, w, x, rows, cols); else mv_q4(out, w, x, rows, cols); }
static void rmsnorm(float *o, const float *x, const float *w, int n) {
    float ss = 0; for (int i = 0; i < n; i++) ss += x[i] * x[i];
    float k = 1 / fsqrt(ss / n + M.eps);
    for (int i = 0; i < n; i++) o[i] = x[i] * k * w[i];
}

/* ---- 計算用の領域 ---- */
static float *x, *xb, *q, *k, *v, *att, *ab, *hb, *hb2, *logits, *kc, *vc;
static XQ8 *xq;
static int pos;
int llm_pos(void) { return pos; }

static void forward(int tok, int p) {
    int H = M.H, HD = M.HD, NH = M.NH, NKV = M.NKV, KV = NKV * HD, FF = M.FF;
    const BQ8 *e = M.emb + (u64)tok * (H / 32);
    for (int b = 0; b < H / 32; b++) { float d = h2f[e[b].d]; for (int i = 0; i < 32; i++) x[b * 32 + i] = d * e[b].q[i]; }
    for (int l = 0; l < M.L; l++) {
        rmsnorm(xb, x, M.layer[l].attnNorm, H);
        quantize(xb, xq, H);
        mv(q, M.layer[l].q, xq, NH * HD, H);
        mv(k, M.layer[l].k, xq, KV, H);
        mv(v, M.layer[l].v, xq, KV, H);
        for (int h = 0; h < NH; h++) rmsnorm(q + h * HD, q + h * HD, M.layer[l].qNorm, HD);
        for (int h = 0; h < NKV; h++) rmsnorm(k + h * HD, k + h * HD, M.layer[l].kNorm, HD);
        const float *c = ropeCos + p * (HD / 2), *s = ropeSin + p * (HD / 2);
        for (int h = 0; h < NH + NKV; h++) { /* 回転位置埋め込み (前半と後半を組にする) */
            float *t = h < NH ? q + h * HD : k + (h - NH) * HD;
            for (int i = 0; i < HD / 2; i++) { float a = t[i], b = t[i + HD / 2]; t[i] = a * c[i] - b * s[i]; t[i + HD / 2] = a * s[i] + b * c[i]; }
        }
        float *kl = kc + ((u64)l * LLM_CTX + p) * KV, *vl = vc + ((u64)l * LLM_CTX + p) * KV;
        for (int i = 0; i < KV; i++) kl[i] = k[i], vl[i] = v[i];
        float scale = 1 / fsqrt((float)HD);
        for (int h = 0; h < NH; h++) {
            int g = h / (NH / NKV);
            const float *qh = q + h * HD;
            float mx = -1e30f;
            for (int t = 0; t <= p; t++) {
                const float *kt = kc + ((u64)l * LLM_CTX + t) * KV + g * HD; float d = 0;
                for (int i = 0; i < HD; i++) d += qh[i] * kt[i];
                att[t] = d * scale; if (att[t] > mx) mx = att[t];
            }
            float sum = 0; for (int t = 0; t <= p; t++) { att[t] = fexp(att[t] - mx); sum += att[t]; }
            float *o = ab + h * HD; for (int i = 0; i < HD; i++) o[i] = 0;
            for (int t = 0; t <= p; t++) { const float *vt = vc + ((u64)l * LLM_CTX + t) * KV + g * HD; float a = att[t] / sum; for (int i = 0; i < HD; i++) o[i] += a * vt[i]; }
        }
        quantize(ab, xq, NH * HD);
        mv(xb, M.layer[l].o, xq, H, NH * HD);
        for (int i = 0; i < H; i++) x[i] += xb[i];
        rmsnorm(xb, x, M.layer[l].postNorm, H);
        quantize(xb, xq, H);
        mv(hb, M.layer[l].gate, xq, FF, H);
        mv(hb2, M.layer[l].up, xq, FF, H);
        for (int i = 0; i < FF; i++) { float g = hb[i]; hb[i] = g / (1 + fexp(-g)) * hb2[i]; } /* SwiGLU */
        quantize(hb, xq, FF);
        mv(xb, M.layer[l].down, xq, H, FF);
        for (int i = 0; i < H; i++) x[i] += xb[i];
    }
    rmsnorm(xb, x, M.finalNorm, H);
    quantize(xb, xq, H);
    mv_q8(logits, M.emb, xq, M.V, H);
}

/* ---- トークナイザー (byte-level BPE) ---- */
#define HBITS 19
static struct MergeSlot { u32 a, b, rank, res; } *mtab;
static int byteTok[256];
static int tImStart, tImEnd, tEnd, tThink, tThinkEnd;
static u32 mhash(u32 a, u32 b) { return (u32)(((u64)a * 0x9E3779B1u ^ (u64)b * 0x85EBCA77u) >> 0) & ((1u << HBITS) - 1); }
static void merge_put(u32 a, u32 b, u32 rank, u32 res) {
    u32 h = mhash(a, b);
    while (mtab[h].rank != 0xFFFFFFFF) { if (mtab[h].a == a && mtab[h].b == b) return; h = (h + 1) & ((1u << HBITS) - 1); }
    mtab[h].a = a; mtab[h].b = b; mtab[h].rank = rank; mtab[h].res = res;
}
static struct MergeSlot *merge_get(u32 a, u32 b) {
    u32 h = mhash(a, b);
    while (mtab[h].rank != 0xFFFFFFFF) { if (mtab[h].a == a && mtab[h].b == b) return &mtab[h]; h = (h + 1) & ((1u << HBITS) - 1); }
    return 0;
}
static int piece_is(int id, const char *s) {
    u32 a = M.tokOff[id], b = M.tokOff[id + 1]; int i = 0;
    for (; s[i]; i++) if (a + i >= b || M.tokBlob[a + i] != (u8)s[i]) return 0;
    return a + i == b;
}
static int find_piece(const char *s) { for (int i = M.V - 1; i >= 0; i--) if (piece_is(i, s)) return i; return -1; }
/* 文字の種類: 0 その他 1 文字 2 数字 3 空白 4 改行 */
static int cls(u32 c) {
    if (c == '\n' || c == '\r') return 4;
    if (c == ' ' || c == '\t' || c == 0x3000) return 3;
    if (c >= '0' && c <= '9') return 2;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
    if (c < 0x80) return 0;
    if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65)) return 0;
    if (c >= 0xFF10 && c <= 0xFF19) return 2;
    return 1;
}
static u32 utf8(const char *s, int n, int *i) {
    u32 c = (u8)s[(*i)++]; int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (more) c &= 0x3F >> more;
    while (more-- && *i < n) c = c << 6 | ((u8)s[(*i)++] & 63);
    return c;
}
static int bpe(const u8 *s, int n, int *out, int cap) { /* 1 かたまりを BPE でトークンにする */
    int ids[512], m = 0;
    for (int i = 0; i < n && m < 512; i++) ids[m++] = byteTok[s[i]];
    for (;;) {
        u32 best = 0xFFFFFFFF; int at = -1, res = 0;
        for (int i = 0; i + 1 < m; i++) { struct MergeSlot *e = merge_get(ids[i], ids[i + 1]); if (e && e->rank < best) best = e->rank, at = i, res = e->res; }
        if (at < 0) break;
        ids[at] = res; for (int i = at + 1; i + 1 < m; i++) ids[i] = ids[i + 1];
        m--;
    }
    int k = 0; for (int i = 0; i < m && k < cap; i++) out[k++] = ids[i];
    return k;
}
/* 文章をトークンにする。分け方は Qwen2 の正規表現に近い簡易版 */
static int encode(const char *s, int n, int *out, int cap) {
    int k = 0, i = 0;
    while (i < n && k < cap) {
        int st = i, j = i; u32 c = utf8(s, n, &j); int t = cls(c);
        if (c == '\'' && j < n) { /* 's 't 're 've 'm 'll 'd */
            static const char *const sf[] = {"s", "t", "re", "ve", "m", "ll", "d"};
            int done = 0;
            for (int f = 0; f < 7 && !done; f++) { int l = 0; while (sf[f][l] && j + l < n && (s[j + l] | 32) == sf[f][l]) l++; if (!sf[f][l]) { i = j + l; done = 1; } }
            if (done) { k += bpe((const u8 *)s + st, i - st, out + k, cap - k); continue; }
        }
        if (t == 1 || ((t == 0 || t == 3) && j < n)) { /* [^\r\n\p{L}\p{N}]?\p{L}+ */
            int p = t == 1 ? st : j, q2 = p, any = 0;
            while (q2 < n) { int r = q2; u32 d = utf8(s, n, &r); if (cls(d) != 1) break; q2 = r; any = 1; }
            if (any) { i = q2; k += bpe((const u8 *)s + st, i - st, out + k, cap - k); continue; }
        }
        if (t == 2) { i = j; k += bpe((const u8 *)s + st, i - st, out + k, cap - k); continue; } /* 数字は 1 文字ずつ */
        if (t == 0 || (t == 3 && c == ' ' && j < n)) { /* ' ?[^\s\p{L}\p{N}]+[\r\n]*' */
            int p = t == 0 ? st : j, q2 = p, any = 0;
            while (q2 < n) { int r = q2; u32 d = utf8(s, n, &r); if (cls(d) != 0) break; q2 = r; any = 1; }
            if (any) { while (q2 < n && (s[q2] == '\n' || s[q2] == '\r')) q2++; i = q2; k += bpe((const u8 *)s + st, i - st, out + k, cap - k); continue; }
        }
        /* 空白と改行 */
        int q2 = st, lastNl = -1;
        while (q2 < n) { int r = q2; u32 d = utf8(s, n, &r); int ct = cls(d); if (ct != 3 && ct != 4) break; if (ct == 4) lastNl = r; q2 = r; }
        if (lastNl > 0) i = lastNl;                                   /* \s*[\r\n]+ */
        else if (q2 < n && q2 - st > 1) i = q2 - 1;                    /* \s+(?!\S): 最後の空白は次の語へ */
        else i = q2 > st ? q2 : j;
        k += bpe((const u8 *)s + st, i - st, out + k, cap - k);
    }
    return k;
}

int llm_encode(const char *s, int n, int *out, int cap) { return encode(s, n, out, cap); }
int llm_load(u8 *data, u64 len) {
    if (len < 256 || data[0] != 'F' || data[1] != 'Q' || data[2] != 'W' || data[3] != '3') { err = "モデルの形式が違います"; return 0; }
    const u32 *h = (const u32 *)(data + 8);
    M.H = h[0]; M.L = h[1]; M.NH = h[2]; M.NKV = h[3]; M.HD = h[4]; M.FF = h[5]; M.V = h[6]; M.nmerge = h[7]; M.theta = (float)h[8]; M.q8 = h[10] == 1;
    __builtin_memcpy(&M.eps, data + 52, 4);
    u64 off[6]; __builtin_memcpy(off, data + 56, 48);
    if (off[5] > len) { err = "モデルのファイルが途中で切れています"; return 0; }
    M.tokOff = (const u32 *)(data + off[0]); M.tokBlob = (const u8 *)(M.tokOff + M.V + 1);
    M.merges = (const u32 *)(data + off[1]);
    for (int i = 0; i < 65536; i++) h2f[i] = f16((u16)i);
    avx2 = cpu_avx2();
    /* 重みの位置 (export と同じ順・64 バイト境界) */
    u64 p = off[2];
        u64 BS_ = 0;
    #define TAKE(type, bytes) ({ p = (p + 63) & ~63ull; type _r = (type)(data + p); p += (bytes); _r; })
    int H = M.H, KV = M.NKV * M.HD, QD = M.NH * M.HD;
    u64 BB = M.q8 ? 34 : 18; (void)BS_;
    M.layer = llm_alloc(sizeof(*M.layer) * (u64)M.L);
    if (!M.layer) { err = "メモリが足りません"; return 0; }
    for (int l = 0; l < M.L; l++) {
        M.layer[l].attnNorm = TAKE(const float *, H * 4);
        M.layer[l].q = TAKE(const void *, (u64)QD * H / 32 * BB);
        M.layer[l].k = TAKE(const void *, (u64)KV * H / 32 * BB);
        M.layer[l].v = TAKE(const void *, (u64)KV * H / 32 * BB);
        M.layer[l].qNorm = TAKE(const float *, M.HD * 4);
        M.layer[l].kNorm = TAKE(const float *, M.HD * 4);
        M.layer[l].o = TAKE(const void *, (u64)H * QD / 32 * BB);
        M.layer[l].postNorm = TAKE(const float *, H * 4);
        M.layer[l].gate = TAKE(const void *, (u64)M.FF * H / 32 * BB);
        M.layer[l].up = TAKE(const void *, (u64)M.FF * H / 32 * BB);
        M.layer[l].down = TAKE(const void *, (u64)H * M.FF / 32 * BB);
    }
    if (((p + 63) & ~63ull) != off[3]) { err = "モデルの中の並びが違います"; return 0; }
    M.finalNorm = (const float *)(data + off[3]);
    M.emb = (const BQ8 *)(data + off[4]);
    /* 計算用の領域 */
    x = llm_alloc(H * 4); xb = llm_alloc((QD > H ? QD : H) * 4); q = llm_alloc(QD * 4); k = llm_alloc(KV * 4); v = llm_alloc(KV * 4);
    att = llm_alloc(LLM_CTX * 4); ab = llm_alloc(QD * 4); hb = llm_alloc(M.FF * 4); hb2 = llm_alloc(M.FF * 4);
    logits = llm_alloc((u64)M.V * 4); xq = llm_alloc((u64)(M.FF > QD ? M.FF : QD) / 32 * sizeof(XQ8));
    kc = llm_alloc((u64)M.L * LLM_CTX * KV * 4); vc = llm_alloc((u64)M.L * LLM_CTX * KV * 4);
    ropeCos = llm_alloc((u64)LLM_CTX * M.HD / 2 * 4); ropeSin = llm_alloc((u64)LLM_CTX * M.HD / 2 * 4);
    mtab = llm_alloc(sizeof(struct MergeSlot) << HBITS);
    if (!x || !xb || !q || !k || !v || !att || !ab || !hb || !hb2 || !logits || !xq || !kc || !vc || !ropeCos || !ropeSin || !mtab) { err = "メモリが足りません"; return 0; }
    for (int i = 0; i < M.HD / 2; i++) {
        double inv = dexp(-(2.0 * i / M.HD) * 13.815510557964274); /* theta^(-2i/HD)、ln(1e6) */
        if (M.theta != 1000000.0f) { double lt = 0, t = M.theta; while (t > 2) t /= 2, lt += 0.6931471805599453; double y = (t - 1) / (t + 1), y2 = y * y, term = y; for (int j = 1; j < 40; j += 2) lt += 2 * term / j, term *= y2; inv = dexp(-(2.0 * i / M.HD) * lt); }
        for (int ps = 0; ps < LLM_CTX; ps++) { double a = ps * inv; ropeSin[ps * (M.HD / 2) + i] = (float)dsin(a); ropeCos[ps * (M.HD / 2) + i] = (float)dsin(a + 1.5707963267948966); }
    }
    for (u32 i = 0; i < (1u << HBITS); i++) mtab[i].rank = 0xFFFFFFFF;
    for (int i = 0; i < M.nmerge; i++) merge_put(M.merges[i * 3], M.merges[i * 3 + 1], (u32)i, M.merges[i * 3 + 2]);
    for (int b = 0; b < 256; b++) byteTok[b] = 0;
    for (int i = 0; i < M.V; i++) if (M.tokOff[i + 1] - M.tokOff[i] == 1 && !byteTok[M.tokBlob[M.tokOff[i]]]) byteTok[M.tokBlob[M.tokOff[i]]] = i;
    tImStart = find_piece("<|im_start|>"); tImEnd = find_piece("<|im_end|>"); tEnd = find_piece("<|endoftext|>");
    tThink = find_piece("<think>"); tThinkEnd = find_piece("</think>");
    if (tImStart < 0 || tImEnd < 0) { err = "特別なトークンがありません"; return 0; }
    pos = 0;
    return 1;
}

/* ---- 会話 ---- */
static u64 rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (u32)(rng >> 32); }
static int sample(void) { /* 温度 0.7、上位 20 個、合計 0.8 まで (Qwen3 の考えない時の推奨値) */
    int top[20]; float val[20]; int n = 0;
    for (int i = 0; i < M.V; i++) {
        float l = logits[i];
        if (n < 20) { int j = n++; while (j > 0 && val[j - 1] < l) val[j] = val[j - 1], top[j] = top[j - 1], j--; val[j] = l; top[j] = i; }
        else if (l > val[19]) { int j = 19; while (j > 0 && val[j - 1] < l) val[j] = val[j - 1], top[j] = top[j - 1], j--; val[j] = l; top[j] = i; }
    }
    float p[20], sum = 0;
    for (int i = 0; i < n; i++) { p[i] = fexp((val[i] - val[0]) / 0.7f); sum += p[i]; }
    float cum = 0; int cut = n;
    for (int i = 0; i < n; i++) { cum += p[i] / sum; if (cum >= 0.8f) { cut = i + 1; break; } }
    float s2 = 0; for (int i = 0; i < cut; i++) s2 += p[i];
    float r = (rnd() / 4294967296.0f) * s2;
    for (int i = 0; i < cut; i++) { r -= p[i]; if (r <= 0) return top[i]; }
    return top[0];
}
static int pending = -1, finished = 1, generated;
void llm_reset(void) { pos = 0; pending = -1; finished = 1; }
static int feed(int tok) { if (pos >= LLM_CTX) return 0; forward(tok, pos++); return 1; }
int llm_begin(const char *s, int n) {
    static int ids[LLM_CTX];
    int m = 0;
    if (pos > 0) { ids[m++] = tImEnd; ids[m++] = byteTok['\n']; } /* 前の返事を閉じる */
    ids[m++] = tImStart;
    m += encode("user\n", 5, ids + m, LLM_CTX - m);
    m += encode(s, n, ids + m, LLM_CTX - m - 16);
    ids[m++] = tImEnd; ids[m++] = byteTok['\n']; ids[m++] = tImStart;
    m += encode("assistant\n", 10, ids + m, LLM_CTX - m);
    if (tThink >= 0) { ids[m++] = tThink; m += encode("\n\n", 2, ids + m, 4); ids[m++] = tThinkEnd; m += encode("\n\n", 2, ids + m, 4); } /* 考えずに答える */
    if (pos + m + 32 >= LLM_CTX) { pos = 0; if (ids[0] == tImEnd) { for (int i = 2; i < m; i++) ids[i - 2] = ids[i]; m -= 2; } }
    if (m + 32 >= LLM_CTX) return 0;
    for (int i = 0; i < m; i++) feed(ids[i]);
    rng ^= (u64)pos * 0x2545F4914F6CDD1Dull;
#if defined(__x86_64__)
    { u32 a, d; __asm__ volatile("rdtsc" : "=a"(a), "=d"(d)); rng ^= (u64)d << 32 | a; }
#endif
    pending = sample(); finished = 0; generated = 0;
    return 1;
}
int llm_step(char *out, int cap) {
    if (finished) return -1;
    int t = pending;
    if (t == tImEnd || t == tEnd || pos >= LLM_CTX - 1 || generated > 400) { finished = 1; return -1; }
    int n = 0;
    if (!(t == tThink || t == tThinkEnd)) { u32 a = M.tokOff[t], b = M.tokOff[t + 1]; for (u32 i = a; i < b && n < cap; i++) out[n++] = (char)M.tokBlob[i]; }
    feed(t); generated++;
    pending = sample();
    return n;
}
