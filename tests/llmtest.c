/* llm.c を Mac / Linux の上で直接試す:
   cc -O2 -Isrc -Isrc/libc tests/llmtest.c src/llm.c -o build/llmtest && build/llmtest model.fqw "質問" */
#include "llm.h"
#include <stdio.h>
#include <stdlib.h>

#include <sys/time.h>
SystemTable *ST;
static int slen8(const char *s) { int n = 0; while (s[n]) n++; return n; }
void *llm_alloc(u64 n) { return calloc(1, n); }
static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *buf = malloc(n); if (fread(buf, 1, n, f) != (size_t)n) return 1; fclose(f);
    if (!llm_load(buf, n)) { printf("load error: %s\n", llm_error()); return 1; }
    printf("[AVX2: %s]\n", llm_has_avx2() ? "yes" : "no");
    for (int a = 2; a < argc; a++) {
        printf("\n> %s\n", argv[a]);
        double t0 = now();
        if (!llm_begin(argv[a], slen8(argv[a]))) { printf("too long\n"); continue; }
        double t1 = now(); int toks = 0; char out[64]; int m;
        while ((m = llm_step(out, 64)) >= 0) { fwrite(out, 1, m, stdout); fflush(stdout); toks++; }
        double t2 = now();
        printf("\n[prefill %.2fs, %d tokens %.1f tok/s, ctx %d]\n", t1 - t0, toks, toks / (t2 - t1), llm_pos());
    }
    return 0;
}
