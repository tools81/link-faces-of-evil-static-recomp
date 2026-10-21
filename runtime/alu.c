/* ALU helpers shared by the recompiled code and the interpreter:
   division, BCD arithmetic and shifts/rotates with exact 68000 flags. */
#include "recomp.h"

void m68k_divu(int reg, uint32_t src, uint32_t addr) {
    src &= 0xFFFF;
    if (src == 0) { m68k_exception(5, addr); return; }
    uint32_t d = D(reg);
    uint32_t q = d / src, r = d % src;
    CF = 0;
    if (q > 0xFFFF) { VF = 1; NF = 1; ZF = 0; return; }
    D(reg) = (r << 16) | q;
    VF = 0; NF = (q >> 15) & 1; ZF = q == 0;
}

void m68k_divs(int reg, uint32_t src, uint32_t addr) {
    int32_t s = (int16_t)src;
    if (s == 0) { m68k_exception(5, addr); return; }
    int32_t d = (int32_t)D(reg);
    if (d == (int32_t)0x80000000 && s == -1) { VF = 1; NF = 1; ZF = 0; CF = 0; return; }
    int32_t q = d / s, r = d % s;
    CF = 0;
    if (q > 32767 || q < -32768) { VF = 1; NF = 1; ZF = 0; return; }
    D(reg) = ((uint32_t)(uint16_t)r << 16) | (uint16_t)q;
    VF = 0; NF = (q >> 15) & 1; ZF = (q & 0xFFFF) == 0;
}

uint8_t BCD_ADD(uint8_t d, uint8_t s) {
    unsigned lo = (s & 15) + (d & 15) + XF;
    unsigned res = (s & 0xF0) + (d & 0xF0) + lo;
    if (lo > 9) res += 6;
    unsigned c = res > 0x99;
    if (c) res += 0x60;
    uint8_t r = (uint8_t)res;
    CF = XF = (uint8_t)c;
    if (r) ZF = 0;
    NF = r >> 7;
    VF = 0;
    return r;
}

uint8_t BCD_SUB(uint8_t d, uint8_t s) {
    int lo = (d & 15) - (s & 15) - XF;
    int res = (d & 0xF0) - (s & 0xF0) + lo;
    int c = 0;
    if (lo < 0) res -= 6;
    if (res < 0) { res -= 0x60; c = 1; }
    uint8_t r = (uint8_t)res;
    CF = XF = (uint8_t)c;
    if (r) ZF = 0;
    NF = r >> 7;
    VF = 0;
    return r;
}

/* ---------------------------------------------------------------- shifts */
static FORCEINLINE uint32_t msk(int bits) { return bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1); }

static FORCEINLINE void nz(uint32_t r, int bits) {
    NF = (r >> (bits - 1)) & 1;
    ZF = (r & msk(bits)) == 0;
}

static uint32_t sh_asl(uint32_t d, uint32_t n, int bits) {
    uint32_t m = msk(bits);
    d &= m;
    if (n == 0) { CF = 0; VF = 0; nz(d, bits); return d; }
    uint32_t r; int c, v;
    if (n < (uint32_t)bits) {
        r = (uint32_t)(((uint64_t)d << n) & m);
        c = (d >> (bits - n)) & 1;
        uint64_t top = (((uint64_t)1 << (n + 1)) - 1) << (bits - 1 - n);
        uint64_t t = d & top;
        v = t != 0 && t != top;
    } else {
        r = 0;
        c = (n == (uint32_t)bits) ? (d & 1) : 0;
        v = d != 0;
    }
    CF = XF = (uint8_t)c; VF = (uint8_t)v; nz(r, bits);
    return r;
}

static uint32_t sh_asr(uint32_t d, uint32_t n, int bits) {
    uint32_t m = msk(bits);
    d &= m;
    int sign = (d >> (bits - 1)) & 1;
    if (n == 0) { CF = 0; VF = 0; nz(d, bits); return d; }
    uint32_t r; int c;
    if (n < (uint32_t)bits) {
        int64_t sd = sign ? (int64_t)d - ((int64_t)1 << bits) : (int64_t)d;
        r = (uint32_t)((uint64_t)(sd >> n) & m);
        c = (d >> (n - 1)) & 1;
    } else {
        r = sign ? m : 0;
        c = sign;
    }
    CF = XF = (uint8_t)c; VF = 0; nz(r, bits);
    return r;
}

static uint32_t sh_lsl(uint32_t d, uint32_t n, int bits) {
    uint32_t m = msk(bits);
    d &= m;
    if (n == 0) { CF = 0; VF = 0; nz(d, bits); return d; }
    uint32_t r; int c;
    if (n < (uint32_t)bits) { r = (uint32_t)(((uint64_t)d << n) & m); c = (d >> (bits - n)) & 1; }
    else { r = 0; c = (n == (uint32_t)bits) ? (d & 1) : 0; }
    CF = XF = (uint8_t)c; VF = 0; nz(r, bits);
    return r;
}

static uint32_t sh_lsr(uint32_t d, uint32_t n, int bits) {
    uint32_t m = msk(bits);
    d &= m;
    if (n == 0) { CF = 0; VF = 0; nz(d, bits); return d; }
    uint32_t r; int c;
    if (n < (uint32_t)bits) { r = d >> n; c = (d >> (n - 1)) & 1; }
    else { r = 0; c = (n == (uint32_t)bits) ? ((d >> (bits - 1)) & 1) : 0; }
    CF = XF = (uint8_t)c; VF = 0; nz(r, bits);
    return r;
}

static uint32_t sh_rol(uint32_t d, uint32_t n, int bits) {
    uint32_t m = msk(bits);
    d &= m;
    if (n == 0) { CF = 0; VF = 0; nz(d, bits); return d; }
    uint32_t k = n % bits;
    uint32_t r = k ? (uint32_t)((((uint64_t)d << k) | (d >> (bits - k))) & m) : d;
    CF = r & 1; VF = 0; nz(r, bits);
    return r;
}

static uint32_t sh_ror(uint32_t d, uint32_t n, int bits) {
    uint32_t m = msk(bits);
    d &= m;
    if (n == 0) { CF = 0; VF = 0; nz(d, bits); return d; }
    uint32_t k = n % bits;
    uint32_t r = k ? (uint32_t)(((d >> k) | ((uint64_t)d << (bits - k))) & m) : d;
    CF = (r >> (bits - 1)) & 1; VF = 0; nz(r, bits);
    return r;
}

static uint32_t sh_roxl(uint32_t d, uint32_t n, int bits) {
    uint32_t m = msk(bits);
    d &= m;
    uint32_t k = n % (bits + 1);
    int x = XF;
    for (uint32_t i = 0; i < k; i++) {
        int c = (d >> (bits - 1)) & 1;
        d = ((d << 1) | x) & m;
        x = c;
    }
    XF = CF = (uint8_t)x; VF = 0; nz(d, bits);
    return d;
}

static uint32_t sh_roxr(uint32_t d, uint32_t n, int bits) {
    uint32_t m = msk(bits);
    d &= m;
    uint32_t k = n % (bits + 1);
    int x = XF;
    for (uint32_t i = 0; i < k; i++) {
        int c = d & 1;
        d = (d >> 1) | ((uint32_t)x << (bits - 1));
        x = c;
    }
    XF = CF = (uint8_t)x; VF = 0; nz(d, bits);
    return d;
}

#define DEF_SH(OP, fn) \
    uint32_t SH_##OP##_B(uint32_t d, uint32_t n) { return fn(d, n, 8); } \
    uint32_t SH_##OP##_W(uint32_t d, uint32_t n) { return fn(d, n, 16); } \
    uint32_t SH_##OP##_L(uint32_t d, uint32_t n) { return fn(d, n, 32); }
DEF_SH(ASL, sh_asl) DEF_SH(ASR, sh_asr) DEF_SH(LSL, sh_lsl) DEF_SH(LSR, sh_lsr)
DEF_SH(ROL, sh_rol) DEF_SH(ROR, sh_ror) DEF_SH(ROXL, sh_roxl) DEF_SH(ROXR, sh_roxr)

