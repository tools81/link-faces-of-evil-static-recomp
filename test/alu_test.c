/* Checks the shared ALU helpers against vectors from an independent bit-serial model. */
#include "recomp.h"
#include <stdio.h>
#include <stdlib.h>
M68K m68k;
uint8_t *RAM;
uint32_t m68k_exception(int v, uint32_t a) { (void)v; (void)a; return 0; }
typedef struct { int op, bits; uint32_t d, n; int x; uint32_t r; int xo, nf, z, v, c; } SV;
static const SV sv[] = {
#include "alu_vectors.h"
};
typedef struct { int a, b, x, ra, ca, rs, cs; } BV;
static const BV bv[] = {
#include "bcd_vectors.h"
};
typedef uint32_t (*ShFn)(uint32_t, uint32_t);
int main(void) {
    ShFn f[8][3] = {
        {SH_ASL_B, SH_ASL_W, SH_ASL_L}, {SH_ASR_B, SH_ASR_W, SH_ASR_L}, {SH_LSL_B, SH_LSL_W, SH_LSL_L},
        {SH_LSR_B, SH_LSR_W, SH_LSR_L}, {SH_ROL_B, SH_ROL_W, SH_ROL_L}, {SH_ROR_B, SH_ROR_W, SH_ROR_L},
        {SH_ROXL_B, SH_ROXL_W, SH_ROXL_L}, {SH_ROXR_B, SH_ROXR_W, SH_ROXR_L}};
    const char *nm[8] = {"asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr"};
    int bad = 0, n = (int)(sizeof sv / sizeof sv[0]);
    for (int i = 0; i < n; i++) {
        const SV *t = &sv[i];
        XF = (uint8_t)t->x;
        int bi = t->bits == 8 ? 0 : t->bits == 16 ? 1 : 2;
        uint32_t r = f[t->op][bi](t->d, t->n);
        int xo_expect = t->xo;
        if ((t->op == 4 || t->op == 5) || (t->n == 0 && t->op < 6)) xo_expect = t->x;  /* X unaffected */
        if (r != t->r || XF != xo_expect || NF != t->nf || ZF != t->z || VF != t->v || CF != t->c) {
            if (bad < 20) printf("%s.%d d=%x n=%u x=%d: got r=%x x%dn%dz%dv%dc%d want r=%x x%dn%dz%dv%dc%d\n",
                nm[t->op], t->bits, t->d, t->n, t->x, r, XF, NF, ZF, VF, CF, t->r, xo_expect, t->nf, t->z, t->v, t->c);
            bad++;
        }
    }
    int nb = (int)(sizeof bv / sizeof bv[0]);
    for (int i = 0; i < nb; i++) {
        const BV *t = &bv[i];
        XF = (uint8_t)t->x;
        uint8_t r = BCD_ADD((uint8_t)t->a, (uint8_t)t->b);
        if (r != t->ra || CF != t->ca) { if (bad < 40) printf("abcd %x+%x+%d got %x c%d want %x c%d\n", t->a, t->b, t->x, r, CF, t->ra, t->ca); bad++; }
        XF = (uint8_t)t->x;
        r = BCD_SUB((uint8_t)t->a, (uint8_t)t->b);
        if (r != t->rs || CF != t->cs) { if (bad < 40) printf("sbcd %x-%x-%d got %x c%d want %x c%d\n", t->a, t->b, t->x, r, CF, t->rs, t->cs); bad++; }
    }
    /* division spot checks */
    D(0) = 100000; m68k_divu(0, 7, 0);
    if (D(0) != ((100000u % 7) << 16 | (100000u / 7))) { printf("divu wrong %x\n", D(0)); bad++; }
    D(0) = (uint32_t)-100000; m68k_divs(0, 7, 0);
    if (D(0) != (((uint32_t)(uint16_t)(-100000 % 7)) << 16 | (uint16_t)(-100000 / 7))) { printf("divs wrong %x\n", D(0)); bad++; }
    D(0) = 0x10000000; m68k_divu(0, 2, 0);
    if (!VF || D(0) != 0x10000000) { printf("divu overflow wrong\n"); bad++; }
    printf("%d shift vectors, %d bcd vectors: %d failures\n", n, nb, bad);
    return bad != 0;
}
