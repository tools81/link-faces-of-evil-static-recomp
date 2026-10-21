/* Differential tester: recompiled instruction semantics vs the interpreter.
   Usage: insn_test <disc image> [trials] */
#include "recomp.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct { uint32_t addr; int len; const char *text; uint32_t (*fn)(void); } InsnTest;
extern const InsnTest insn_tests[];
extern const int insn_test_count;

M68K m68k;
uint8_t *RAM;
uint32_t a5_expected;
const uint32_t recomp_code_lo = 0x100048, recomp_code_hi = 0x120056;
int log_level = 0;

uint32_t interp_step(uint32_t pc);

/* ---- stubs for the runtime pieces we don't link */
static int exc_flag;
void logf_(int lvl, const char *fmt, ...) { (void)lvl; va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); }
void fatal(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); exit(2); }
const char *func_name(uint32_t pc) { (void)pc; return "?"; }
uint32_t m68k_exception(int vec, uint32_t addr) { (void)addr; exc_flag = vec; return 0; }
uint32_t m68k_call(uint32_t t) { return t; }
uint32_t m68k_trap(int n, uint32_t f, uint32_t next) { (void)n; (void)f; return next; }
uint32_t os9_syscall(uint32_t code, uint32_t next) { (void)code; return next; }
uint32_t recomp_bad_entry(uint32_t pc, uint32_t f) { (void)f; return pc; }
void a5_check(uint32_t a) { (void)a; }

/* ---- write log */
typedef struct { uint32_t a; uint8_t old; } WLog;
static WLog wlog[4096];
static int nwlog;
void test_log_write(uint32_t a, int n) {
    for (int k = 0; k < n && nwlog < 4096; k++) {
        uint32_t x = (a + k) & MEM_MASK;
        wlog[nwlog].a = x;
        wlog[nwlog].old = RAM[x];
        nwlog++;
    }
}

typedef struct { uint32_t a; uint8_t v; } MemRes;
static void collect(MemRes *out, int *n) {
    *n = 0;
    for (int i = 0; i < nwlog; i++) {
        int dup = 0;
        for (int j = 0; j < *n; j++) if (out[j].a == wlog[i].a) { dup = 1; break; }
        if (!dup) { out[*n].a = wlog[i].a; out[*n].v = RAM[wlog[i].a]; (*n)++; }
    }
    for (int i = nwlog - 1; i >= 0; i--) RAM[wlog[i].a] = wlog[i].old;
    nwlog = 0;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
static uint32_t rnd_data(void) {
    static const uint32_t edge[] = {0, 1, 2, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFF, 0x8000, 0x7FFF,
                                    0x80, 0x7F, 0xFF, 0x10000, 0xFFFF8000, 0x99, 0x09, 0x100, 31, 32, 33, 63, 64};
    switch (rnd() % 4) {
    case 0: return edge[rnd() % (sizeof edge / sizeof edge[0])];
    case 1: return rnd() & 0xFF;
    case 2: return rnd() & 0xFFFF;
    default: return rnd() ^ (rnd() << 16);
    }
}

typedef struct { uint32_t d[8], a[8]; uint8_t f[5]; uint32_t pc; int exc; } State;

static void save(State *s, uint32_t pc) {
    memcpy(s->d, m68k.d, 32); memcpy(s->a, m68k.a, 32);
    s->f[0] = XF; s->f[1] = NF; s->f[2] = ZF; s->f[3] = VF; s->f[4] = CF;
    s->pc = pc; s->exc = exc_flag;
}
static void load(const State *s) {
    memcpy(m68k.d, s->d, 32); memcpy(m68k.a, s->a, 32);
    XF = s->f[0]; NF = s->f[1]; ZF = s->f[2]; VF = s->f[3]; CF = s->f[4];
}

int disc_open(const char *path);
const DiscFile *disc_find(const char *name);
int disc_read_file(const DiscFile *f, uint8_t **out, uint32_t *len);

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: insn_test <image.bin> [trials]\n"); return 1; }
    int trials = argc > 2 ? atoi(argv[2]) : 200;
    RAM = (uint8_t *)calloc(MEM_SIZE + 16, 1);
    if (disc_open(argv[1])) { fprintf(stderr, "cannot open disc\n"); return 1; }
    uint8_t *mod; uint32_t len;
    disc_read_file(disc_find("cdi_link"), &mod, &len);
    memcpy(RAM + 0x100000, mod, len);
    /* fill the rest of memory with a deterministic pattern */
    for (uint32_t i = 0x200000; i < 0x800000; i++) RAM[i] = (uint8_t)(i * 2654435761u >> 24);
    int fails = 0, failed_tests = 0;
    for (int t = 0; t < insn_test_count; t++) {
        const InsnTest *T = &insn_tests[t];
        int tfail = 0;
        for (int k = 0; k < trials && !tfail; k++) {
            State init, sa, sb;
            for (int r = 0; r < 8; r++) m68k.d[r] = rnd_data();
            for (int r = 0; r < 8; r++) m68k.a[r] = 0x300000 + ((rnd() % 0x80000) & ~1u);
            if (rnd() % 4 == 0) m68k.a[rnd() % 7] = rnd_data();   /* occasionally a data-like address */
            SET_CCR(rnd());
            save(&init, 0);
            MemRes ma[1024], mb[1024];
            int na, nb;
            /* recompiled */
            exc_flag = 0;
            uint32_t pca = T->fn();
            save(&sa, pca);
            collect(ma, &na);
            /* interpreter */
            load(&init);
            exc_flag = 0;
            uint32_t pcb = interp_step(T->addr);
            save(&sb, pcb);
            collect(mb, &nb);
            int bad = 0;
            if (memcmp(sa.d, sb.d, 32) || memcmp(sa.a, sb.a, 32) || sa.pc != sb.pc || sa.exc != sb.exc) bad = 1;
            if (memcmp(sa.f, sb.f, 5)) bad = 1;
            /* memory: every address written by either side must end with the same value */
            for (int i = 0; i < na && !bad; i++) {
                uint8_t vb = RAM[ma[i].a];
                for (int j = 0; j < nb; j++) if (mb[j].a == ma[i].a) { vb = mb[j].v; break; }
                if (vb != ma[i].v) bad = 1;
            }
            for (int j = 0; j < nb && !bad; j++) {
                uint8_t va = RAM[mb[j].a];
                for (int i = 0; i < na; i++) if (ma[i].a == mb[j].a) { va = ma[i].v; break; }
                if (va != mb[j].v) bad = 1;
            }
            if (bad) {
                tfail = 1;
                fails++;
                if (failed_tests < 60) {
                    printf("MISMATCH %06x %s\n", T->addr, T->text);
                    printf("  init: d=%08x %08x %08x %08x %08x %08x %08x %08x ccr x%dn%dz%dv%dc%d\n",
                           init.d[0], init.d[1], init.d[2], init.d[3], init.d[4], init.d[5], init.d[6], init.d[7],
                           init.f[0], init.f[1], init.f[2], init.f[3], init.f[4]);
                    printf("        a=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                           init.a[0], init.a[1], init.a[2], init.a[3], init.a[4], init.a[5], init.a[6], init.a[7]);
                    const State *S[2] = {&sa, &sb};
                    const char *nm[2] = {"recomp", "interp"};
                    for (int q = 0; q < 2; q++) {
                        printf("  %s: pc=%06x exc=%d d=", nm[q], S[q]->pc, S[q]->exc);
                        for (int r = 0; r < 8; r++) printf("%08x ", S[q]->d[r]);
                        printf("a=");
                        for (int r = 0; r < 8; r++) printf("%08x ", S[q]->a[r]);
                        printf("x%dn%dz%dv%dc%d\n", S[q]->f[0], S[q]->f[1], S[q]->f[2], S[q]->f[3], S[q]->f[4]);
                    }
                    printf("  mem recomp:");
                    for (int i = 0; i < na && i < 8; i++) printf(" %06x=%02x", ma[i].a, ma[i].v);
                    printf("\n  mem interp:");
                    for (int i = 0; i < nb && i < 8; i++) printf(" %06x=%02x", mb[i].a, mb[i].v);
                    printf("\n");
                }
                failed_tests++;
            }
        }
    }
    printf("%d instruction tests, %d failed\n", insn_test_count, failed_tests);
    return failed_tests ? 1 : 0;
}
