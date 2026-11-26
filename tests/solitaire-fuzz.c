int puts(const char *); int printf(const char *, ...); void exit(int); int rand(void); void *memmove(void *, const void *, unsigned long);
#include "solitaire.c"
SystemTable *ST;
static Status gt(Time *t, void *c) { (void)c; memset(t, 0, sizeof *t); t->ns = rand(); return 0; }
void mcopy(void *d, const void *s, u64 n) { memmove(d, s, n); }
void fill(int a,int b,int c,int d,u32 e){} int glyph(int a,int b,c16 c,u32 d){return 8;} int text(int a,int b,const c16*s,u32 d){return a;}
void scopy(c16*d,const c16*s,int c){} void scat(c16*d,const c16*s,int c){} void snum(c16*d,i64 v,int w,int c){}
static void check(void) {
    int seen[52] = {0}, total = 0;
    for (int p = 0; p < PILES; p++) {
        if (g.n[p] > 24 || (p >= 6 && g.n[p] && g.down[p] >= g.n[p])) { printf("bad pile %d\n", p); exit(1); }
        for (int i = 0; i < g.n[p]; i++) { seen[g.c[p][i]]++; total++; }
        if (p >= 2 && p < 6) for (int i = 0; i < g.n[p]; i++) if (rank(g.c[p][i]) != i || suit(g.c[p][i]) != suit(g.c[p][0])) { puts("bad foundation"); exit(1); }
        if (p >= 6) for (int i = g.down[p] + 1; i < g.n[p]; i++) { u8 a = g.c[p][i-1], b = g.c[p][i]; if (red(a) == red(b) || rank(a) != rank(b) + 1) { puts("bad tableau"); exit(1); } }
    }
    if (total != 52) { puts("count"); exit(1); }
    for (int i = 0; i < 52; i++) if (seen[i] != 1) { puts("dup"); exit(1); }
}
int main(void) {
    static RuntimeServices rt; static SystemTable st; rt.GetTime = gt; st.rt = &rt; ST = &st;
    const char keys[] = "\r\r\r\r au";
    int wins = 0;
    for (int game = 0; game < 3000; game++) {
        solitaire_open();
        for (int s = 0; s < 3000; s++) {
            Key k = {0, 0};
            int r = rand() % 4;
            if (r == 0) k.scan = 1 + rand() % 4; else if (r < 3) k.ch = keys[rand() % 7]; else solitaire_click(rand() % 394, rand() % 480);
            if (r < 3) solitaire_key(k);
            check();
        }
        wins += won();
    }
    printf("OK 9M operations, wins=%d\n", wins);
}
