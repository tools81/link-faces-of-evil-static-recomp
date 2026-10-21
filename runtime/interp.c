/* MC68000 interpreter used as a fallback for code that is not part of the
   statically recompiled module (the game loads "compiled sprite" routines
   from disc into RAM and calls them).  Executes until control returns into
   the recompiled code range, then hands the pc back to the dispatcher. */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include <string.h>

/* memory writes go through these so smooth motion can capture sprite pixels */
static FORCEINLINE void IWB(uint32_t a, uint32_t v) { if (spr_capturing()) spr_write(a, 1, v); WB(a, v); }
static FORCEINLINE void IWW(uint32_t a, uint32_t v) { if (spr_capturing()) spr_write(a, 2, v); WW(a, v); }
static FORCEINLINE void IWL(uint32_t a, uint32_t v) { if (spr_capturing()) spr_write(a, 4, v); WL(a, v); }

static uint32_t ipc;        /* interpreter pc */
static int istop;           /* set when pc leaves interpreted space */
static int ibranch;         /* last instruction was a branch that may yield (same rule as BUDGET) */
int g_interp_all;           /* debug: interpret everything, including recompiled code */

#define SZB 0
#define SZW 1
#define SZL 2
static const uint32_t SZMASK[3] = {0xFF, 0xFFFF, 0xFFFFFFFF};
static const uint32_t SZMSB[3] = {0x80, 0x8000, 0x80000000};
static const int SZBYTES[3] = {1, 2, 4};

static FORCEINLINE uint16_t fetch16(void) { uint16_t v = RW(ipc); ipc += 2; return v; }
static FORCEINLINE uint32_t fetch32(void) { uint32_t v = RL(ipc); ipc += 4; return v; }

static FORCEINLINE int in_static(uint32_t pc) {
    return !g_interp_all && pc >= recomp_code_lo && pc < recomp_code_hi;
}

typedef struct { int kind; uint32_t addr; int reg; uint32_t imm; } EARef;  /* kind: 0 dn 1 an 2 mem 3 imm */

static uint32_t ext_index(uint32_t base) {
    uint16_t ext = fetch16();
    int r = (ext >> 12) & 15;
    uint32_t x = r < 8 ? D(r) : A(r - 8);
    if (!(ext & 0x800)) x = (uint32_t)(int32_t)(int16_t)x;
    return base + (uint32_t)(int32_t)(int8_t)(ext & 0xFF) + x;
}

static int ea_get(int mode, int reg, int sz, EARef *e) {
    int n = SZBYTES[sz];
    switch (mode) {
    case 0: e->kind = 0; e->reg = reg; return 1;
    case 1: e->kind = 1; e->reg = reg; return 1;
    case 2: e->kind = 2; e->addr = A(reg); return 1;
    case 3: e->kind = 2; e->addr = A(reg); A(reg) += (reg == 7 && n == 1) ? 2 : n; return 1;
    case 4: A(reg) -= (reg == 7 && n == 1) ? 2 : n; e->kind = 2; e->addr = A(reg); return 1;
    case 5: e->kind = 2; e->addr = A(reg) + (uint32_t)(int32_t)(int16_t)fetch16(); return 1;
    case 6: e->kind = 2; e->addr = ext_index(A(reg)); return 1;
    case 7:
        switch (reg) {
        case 0: e->kind = 2; e->addr = (uint32_t)(int32_t)(int16_t)fetch16(); return 1;
        case 1: e->kind = 2; e->addr = fetch32(); return 1;
        case 2: { uint32_t b = ipc; e->kind = 2; e->addr = b + (uint32_t)(int32_t)(int16_t)fetch16(); return 1; }
        case 3: { uint32_t b = ipc; e->kind = 2; e->addr = ext_index(b); return 1; }
        case 4:
            e->kind = 3;
            if (sz == SZL) e->imm = fetch32();
            else e->imm = fetch16() & SZMASK[sz];
            return 1;
        }
    }
    return 0;
}

/* address only (control addressing modes) */
static uint32_t ea_addr_ctl(int mode, int reg) {
    EARef e;
    ea_get(mode, reg, SZL, &e);
    return e.addr;
}

static uint32_t ea_read(EARef *e, int sz) {
    switch (e->kind) {
    case 0: return D(e->reg) & SZMASK[sz];
    case 1: return A(e->reg) & SZMASK[sz];
    case 2: return sz == SZB ? RB(e->addr) : sz == SZW ? RW(e->addr) : RL(e->addr);
    default: return e->imm;
    }
}

static void ea_write(EARef *e, int sz, uint32_t v) {
    switch (e->kind) {
    case 0:
        if (sz == SZL) D(e->reg) = v;
        else D(e->reg) = (D(e->reg) & ~SZMASK[sz]) | (v & SZMASK[sz]);
        break;
    case 1: A(e->reg) = sz == SZW ? (uint32_t)(int32_t)(int16_t)v : v; break;
    case 2:
        if (sz == SZB) IWB(e->addr, v); else if (sz == SZW) IWW(e->addr, v); else IWL(e->addr, v);
        break;
    }
}

static FORCEINLINE void set_nz(uint32_t r, int sz) {
    r &= SZMASK[sz];
    NF = (r & SZMSB[sz]) != 0;
    ZF = r == 0;
}
static FORCEINLINE void set_logic(uint32_t r, int sz) { set_nz(r, sz); VF = 0; CF = 0; }

static uint32_t do_add(uint32_t d, uint32_t s, int sz, int x, int setx) {
    uint32_t m = SZMASK[sz], msb = SZMSB[sz];
    d &= m; s &= m;
    uint32_t r = (d + s + (uint32_t)x) & m;
    CF = (((s & d) | (~r & (s | d))) & msb) != 0;
    VF = (((s ^ r) & (d ^ r)) & msb) != 0;
    if (setx) XF = CF;
    NF = (r & msb) != 0;
    ZF = r == 0;
    return r;
}

static uint32_t do_sub(uint32_t d, uint32_t s, int sz, int x, int setx) {
    uint32_t m = SZMASK[sz], msb = SZMSB[sz];
    d &= m; s &= m;
    uint32_t r = (d - s - (uint32_t)x) & m;
    CF = (((s & ~d) | (r & ~d) | (s & r)) & msb) != 0;
    VF = (((s ^ d) & (r ^ d)) & msb) != 0;
    if (setx) XF = CF;
    NF = (r & msb) != 0;
    ZF = r == 0;
    return r;
}

static int test_cc(int cc) {
    switch (cc) {
    case 0: return 1;
    case 1: return 0;
    case 2: return !CF && !ZF;
    case 3: return CF || ZF;
    case 4: return !CF;
    case 5: return CF;
    case 6: return !ZF;
    case 7: return ZF;
    case 8: return !VF;
    case 9: return VF;
    case 10: return !NF;
    case 11: return NF;
    case 12: return NF == VF;
    case 13: return NF != VF;
    case 14: return !ZF && NF == VF;
    default: return ZF || NF != VF;
    }
}

static void interp_fatal(uint16_t op) {
    fatal("interpreter: unsupported opcode %04x at %06x", op, ipc - 2);
}

/* jump/call helpers */
static void do_jump(uint32_t t) {
    ipc = t;
    if (in_static(t)) istop = 1;
}

static void do_call(uint32_t t) {
    uint32_t ret = ipc;
    A(7) -= 4;
    IWL(A(7), ret);
    if (in_static(t)) {
        /* call into recompiled code; continue interpreting on return */
        uint32_t r = m68k_call(t);
        ipc = ret;
        if (r != ret) { ipc = r; istop = 1; }
        return;
    }
    ipc = t;
}

static void exec_one(void) {
    uint16_t op = fetch16();
    int hi = op >> 12;
    int mode = (op >> 3) & 7, reg = op & 7, reg9 = (op >> 9) & 7, ss = (op >> 6) & 3;
    EARef e, e2;
    uint32_t s, d, r;
    switch (hi) {
    case 0x0: {
        if (op & 0x100) {
            if (mode == 1) { /* movep */
                uint32_t a = A(reg) + (uint32_t)(int32_t)(int16_t)fetch16();
                switch (ss) {
                case 0: D(reg9) = (D(reg9) & 0xFFFF0000u) | (RB(a) << 8) | RB(a + 2); break;
                case 1: D(reg9) = (RB(a) << 24) | (RB(a + 2) << 16) | (RB(a + 4) << 8) | RB(a + 6); break;
                case 2: IWB(a, D(reg9) >> 8); IWB(a + 2, D(reg9)); break;
                case 3: IWB(a, D(reg9) >> 24); IWB(a + 2, D(reg9) >> 16); IWB(a + 4, D(reg9) >> 8); IWB(a + 6, D(reg9)); break;
                }
                return;
            }
            uint32_t bit = D(reg9);
            ea_get(mode, reg, SZB, &e);
            int sz = e.kind == 0 ? SZL : SZB;
            bit &= sz == SZL ? 31 : 7;
            d = ea_read(&e, sz);
            ZF = !((d >> bit) & 1);
            if (ss == 1) ea_write(&e, sz, d ^ (1u << bit));
            else if (ss == 2) ea_write(&e, sz, d & ~(1u << bit));
            else if (ss == 3) ea_write(&e, sz, d | (1u << bit));
            return;
        }
        int t = reg9;
        if (t == 4) {
            uint32_t bit = fetch16() & 0xFF;
            ea_get(mode, reg, SZB, &e);
            int sz = e.kind == 0 ? SZL : SZB;
            bit &= sz == SZL ? 31 : 7;
            d = ea_read(&e, sz);
            ZF = !((d >> bit) & 1);
            if (ss == 1) ea_write(&e, sz, d ^ (1u << bit));
            else if (ss == 2) ea_write(&e, sz, d & ~(1u << bit));
            else if (ss == 3) ea_write(&e, sz, d | (1u << bit));
            return;
        }
        if (ss == 3) interp_fatal(op);
        if (mode == 7 && reg == 4 && (t == 0 || t == 1 || t == 5)) {
            uint16_t v = fetch16();
            uint16_t sr = ss == 0 ? GET_CCR() : GET_SR();
            if (t == 0) sr |= v; else if (t == 1) sr &= v; else sr ^= v;
            SET_CCR(sr);
            if (ss == 1) SET_SRH(sr >> 8);
            return;
        }
        int sz = ss;
        s = sz == SZL ? fetch32() : fetch16() & SZMASK[sz];
        ea_get(mode, reg, sz, &e);
        d = ea_read(&e, sz);
        switch (t) {
        case 0: r = d | s; ea_write(&e, sz, r); set_logic(r, sz); break;
        case 1: r = d & s; ea_write(&e, sz, r); set_logic(r, sz); break;
        case 2: r = do_sub(d, s, sz, 0, 1); ea_write(&e, sz, r); break;
        case 3: r = do_add(d, s, sz, 0, 1); ea_write(&e, sz, r); break;
        case 5: r = d ^ s; ea_write(&e, sz, r); set_logic(r, sz); break;
        case 6: do_sub(d, s, sz, 0, 0); break;
        default: interp_fatal(op);
        }
        return;
    }
    case 0x1: case 0x2: case 0x3: {
        int sz = hi == 1 ? SZB : hi == 3 ? SZW : SZL;
        ea_get(mode, reg, sz, &e);
        s = ea_read(&e, sz);
        int dmode = (op >> 6) & 7;
        if (dmode == 1) { A(reg9) = sz == SZW ? (uint32_t)(int32_t)(int16_t)s : s; return; }
        ea_get(dmode, reg9, sz, &e2);
        ea_write(&e2, sz, s);
        set_logic(s, sz);
        return;
    }
    case 0x4: {
        if (op == 0x4E71) return;                      /* nop */
        if (op == 0x4E75) { uint32_t ra = RL(A(7)); A(7) += 4; do_jump(ra); return; }
        if (op == 0x4E77) { SET_CCR(RW(A(7))); uint32_t ra = RL(A(7) + 2); A(7) += 6; do_jump(ra); return; }
        if ((op & 0xFFF0) == 0x4E40) {
            if ((op & 15) == 0) {
                uint16_t code = fetch16();
                uint32_t next = ipc;
                uint32_t r2 = os9_syscall(code, next);
                ipc = next;
                if (r2 != next) { ipc = r2; istop = 1; }
                return;
            }
            interp_fatal(op);
        }
        if ((op & 0xFFF8) == 0x4E50) { /* link */
            int16_t disp = (int16_t)fetch16();
            A(7) -= 4; IWL(A(7), A(reg)); A(reg) = A(7); A(7) += (uint32_t)(int32_t)disp;
            return;
        }
        if ((op & 0xFFF8) == 0x4E58) { A(7) = A(reg); A(reg) = RL(A(7)); A(7) += 4; return; }
        if ((op & 0xFFC0) == 0x4E80) { uint32_t t = ea_addr_ctl(mode, reg); do_call(t); return; }
        if ((op & 0xFFC0) == 0x4EC0) {
            uint32_t t = ea_addr_ctl(mode, reg);
            if (mode == 7 && (reg == 2 || reg == 3)) ibranch = 1;   /* pc-relative jmp / switch table */
            do_jump(t);
            return;
        }
        if (op & 0x100) {
            if (ss == 3) { A(reg9) = ea_addr_ctl(mode, reg); return; }          /* lea */
            if (ss == 2) { /* chk */
                ea_get(mode, reg, SZW, &e);
                int16_t b = (int16_t)ea_read(&e, SZW), v = (int16_t)D(reg9);
                if (v < 0 || v > b) m68k_exception(6, ipc);
                return;
            }
            interp_fatal(op);
        }
        int sub = (op >> 8) & 15;
        switch (sub) {
        case 0x0:
            if (ss == 3) { ea_get(mode, reg, SZW, &e); ea_write(&e, SZW, GET_SR()); return; }
            ea_get(mode, reg, ss, &e);
            d = ea_read(&e, ss);
            {
                uint8_t z = ZF;
                r = do_sub(0, d, ss, XF, 1);
                ZF = r ? 0 : z;
            }
            ea_write(&e, ss, r);
            return;
        case 0x2:
            ea_get(mode, reg, ss, &e); ea_write(&e, ss, 0);
            NF = 0; ZF = 1; VF = 0; CF = 0;
            return;
        case 0x4:
            if (ss == 3) { ea_get(mode, reg, SZW, &e); SET_CCR(ea_read(&e, SZW)); return; }
            ea_get(mode, reg, ss, &e);
            d = ea_read(&e, ss);
            r = do_sub(0, d, ss, 0, 1);
            ea_write(&e, ss, r);
            return;
        case 0x6:
            if (ss == 3) { ea_get(mode, reg, SZW, &e); s = ea_read(&e, SZW); SET_CCR(s); SET_SRH(s >> 8); return; }
            ea_get(mode, reg, ss, &e);
            r = ~ea_read(&e, ss) & SZMASK[ss];
            ea_write(&e, ss, r);
            set_logic(r, ss);
            return;
        case 0x8:
            if (ss == 0) { /* nbcd */
                ea_get(mode, reg, SZB, &e);
                ea_write(&e, SZB, BCD_SUB(0, (uint8_t)ea_read(&e, SZB)));
                return;
            }
            if (ss == 1) {
                if (mode == 0) { D(reg) = (D(reg) >> 16) | (D(reg) << 16); set_logic(D(reg), SZL); return; }
                uint32_t a = ea_addr_ctl(mode, reg);
                A(7) -= 4; IWL(A(7), a);
                return;
            }
            if (mode == 0) {
                if (ss == 2) { D(reg) = (D(reg) & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)D(reg); set_logic(D(reg), SZW); }
                else { D(reg) = (uint32_t)(int32_t)(int16_t)D(reg); set_logic(D(reg), SZL); }
                return;
            }
            { /* movem regs -> mem */
                uint16_t mask = fetch16();
                int sz = ss == 2 ? SZW : SZL, n = SZBYTES[sz];
                if (mode == 4) {
                    uint32_t a = A(reg);
                    for (int k = 15; k >= 0; k--) {
                        if (!(mask & (1u << (15 - k)))) continue;
                        uint32_t v = k < 8 ? D(k) : A(k - 8);
                        a -= n;
                        if (sz == SZW) IWW(a, v); else IWL(a, v);
                    }
                    A(reg) = a;
                } else {
                    uint32_t a = ea_addr_ctl(mode, reg);
                    for (int k = 0; k < 16; k++) {
                        if (!(mask & (1u << k))) continue;
                        uint32_t v = k < 8 ? D(k) : A(k - 8);
                        if (sz == SZW) IWW(a, v); else IWL(a, v);
                        a += n;
                    }
                }
            }
            return;
        case 0xA:
            if (op == 0x4AFC) { m68k_exception(4, ipc - 2); return; }
            if (ss == 3) {
                ea_get(mode, reg, SZB, &e); d = ea_read(&e, SZB); set_logic(d, SZB); ea_write(&e, SZB, d | 0x80);
                return;
            }
            ea_get(mode, reg, ss, &e);
            set_logic(ea_read(&e, ss), ss);
            return;
        case 0xC:
            if (ss >= 2) { /* movem mem -> regs */
                uint16_t mask = fetch16();
                int sz = ss == 2 ? SZW : SZL, n = SZBYTES[sz];
                uint32_t a = mode == 3 ? A(reg) : ea_addr_ctl(mode, reg);
                for (int k = 0; k < 16; k++) {
                    if (!(mask & (1u << k))) continue;
                    uint32_t v = sz == SZW ? (uint32_t)(int32_t)(int16_t)RW(a) : RL(a);
                    if (k < 8) D(k) = v; else A(k - 8) = v;
                    a += n;
                }
                if (mode == 3) A(reg) = a;
                return;
            }
            break;
        }
        interp_fatal(op);
        return;
    }
    case 0x5: {
        if (ss == 3) {
            int cc = (op >> 8) & 15;
            if (mode == 1) { /* dbcc */
                uint32_t base = ipc;
                int16_t disp = (int16_t)fetch16();
                if (!test_cc(cc)) {
                    uint16_t c = (uint16_t)D(reg) - 1;
                    D(reg) = (D(reg) & 0xFFFF0000u) | c;
                    if (c != 0xFFFF) { ibranch = 1; do_jump(base + (uint32_t)(int32_t)disp); }
                }
                return;
            }
            ea_get(mode, reg, SZB, &e);
            ea_write(&e, SZB, test_cc(cc) ? 0xFF : 0);
            return;
        }
        uint32_t q = reg9 ? reg9 : 8;
        if (mode == 1) { if (op & 0x100) A(reg) -= q; else A(reg) += q; return; }
        ea_get(mode, reg, ss, &e);
        d = ea_read(&e, ss);
        r = (op & 0x100) ? do_sub(d, q, ss, 0, 1) : do_add(d, q, ss, 0, 1);
        ea_write(&e, ss, r);
        return;
    }
    case 0x6: {
        int cc = (op >> 8) & 15;
        uint32_t base = ipc;
        int32_t disp = (int8_t)(op & 0xFF);
        if (disp == 0) disp = (int16_t)fetch16();
        uint32_t t = base + (uint32_t)disp;
        if (cc == 1) do_call(t);
        else if (test_cc(cc)) { ibranch = 1; do_jump(t); }
        return;
    }
    case 0x7:
        D(reg9) = (uint32_t)(int32_t)(int8_t)(op & 0xFF);
        set_logic(D(reg9), SZL);
        return;
    case 0x8: case 0xC: {
        if (ss == 3) {
            ea_get(mode, reg, SZW, &e);
            s = ea_read(&e, SZW);
            if (hi == 0xC) {
                if (op & 0x100) D(reg9) = (uint32_t)((int32_t)(int16_t)D(reg9) * (int32_t)(int16_t)s);
                else D(reg9) = (uint32_t)(uint16_t)D(reg9) * (uint16_t)s;
                set_logic(D(reg9), SZL);
            } else {
                if (op & 0x100) m68k_divs(reg9, s, ipc); else m68k_divu(reg9, s, ipc);
            }
            return;
        }
        if ((op & 0x1F0) == 0x100) { /* abcd / sbcd */
            uint8_t sv, dv;
            if (op & 8) {
                A(reg) -= (reg == 7) ? 2 : 1; sv = RB(A(reg));
                A(reg9) -= (reg9 == 7) ? 2 : 1; dv = RB(A(reg9));
                IWB(A(reg9), hi == 0xC ? BCD_ADD(dv, sv) : BCD_SUB(dv, sv));
            } else {
                sv = (uint8_t)D(reg); dv = (uint8_t)D(reg9);
                uint8_t res = hi == 0xC ? BCD_ADD(dv, sv) : BCD_SUB(dv, sv);
                D(reg9) = (D(reg9) & 0xFFFFFF00u) | res;
            }
            return;
        }
        if (hi == 0xC && ((op & 0x1F8) == 0x140 || (op & 0x1F8) == 0x148 || (op & 0x1F8) == 0x188)) {
            uint32_t *x, *y;
            int m = op & 0x1F8;
            if (m == 0x140) { x = &D(reg9); y = &D(reg); }
            else if (m == 0x148) { x = &A(reg9); y = &A(reg); }
            else { x = &D(reg9); y = &A(reg); }
            uint32_t t = *x; *x = *y; *y = t;
            return;
        }
        ea_get(mode, reg, ss, &e);
        if (op & 0x100) {
            d = ea_read(&e, ss);
            r = hi == 8 ? (d | D(reg9)) : (d & D(reg9));
            ea_write(&e, ss, r);
        } else {
            s = ea_read(&e, ss);
            r = hi == 8 ? (D(reg9) | s) : (D(reg9) & s);
            e2.kind = 0; e2.reg = reg9;
            ea_write(&e2, ss, r);
        }
        set_logic(r, ss);
        return;
    }
    case 0x9: case 0xD: {
        int add = hi == 0xD;
        if (ss == 3) {
            int sz = (op & 0x100) ? SZL : SZW;
            ea_get(mode, reg, sz, &e);
            s = ea_read(&e, sz);
            if (sz == SZW) s = (uint32_t)(int32_t)(int16_t)s;
            if (add) A(reg9) += s; else A(reg9) -= s;
            return;
        }
        if ((op & 0x130) == 0x100) { /* addx / subx */
            int sz = ss;
            int z = ZF;
            if (op & 8) {
                int n = SZBYTES[sz];
                A(reg) -= (reg == 7 && n == 1) ? 2 : n;
                EARef se = {2, A(reg), 0, 0};
                s = ea_read(&se, sz);
                A(reg9) -= (reg9 == 7 && n == 1) ? 2 : n;
                EARef de = {2, A(reg9), 0, 0};
                d = ea_read(&de, sz);
                r = add ? do_add(d, s, sz, XF, 1) : do_sub(d, s, sz, XF, 1);
                ea_write(&de, sz, r);
            } else {
                s = D(reg) & SZMASK[sz]; d = D(reg9) & SZMASK[sz];
                r = add ? do_add(d, s, sz, XF, 1) : do_sub(d, s, sz, XF, 1);
                EARef de = {0, 0, reg9, 0};
                ea_write(&de, sz, r);
            }
            ZF = r ? 0 : (uint8_t)z;
            return;
        }
        ea_get(mode, reg, ss, &e);
        if (op & 0x100) {
            d = ea_read(&e, ss);
            r = add ? do_add(d, D(reg9), ss, 0, 1) : do_sub(d, D(reg9), ss, 0, 1);
            ea_write(&e, ss, r);
        } else {
            s = ea_read(&e, ss);
            r = add ? do_add(D(reg9), s, ss, 0, 1) : do_sub(D(reg9), s, ss, 0, 1);
            e2.kind = 0; e2.reg = reg9;
            ea_write(&e2, ss, r);
        }
        return;
    }
    case 0xB: {
        if (ss == 3) { /* cmpa */
            int sz = (op & 0x100) ? SZL : SZW;
            ea_get(mode, reg, sz, &e);
            s = ea_read(&e, sz);
            if (sz == SZW) s = (uint32_t)(int32_t)(int16_t)s;
            do_sub(A(reg9), s, SZL, 0, 0);
            return;
        }
        if (op & 0x100) {
            if (mode == 1) { /* cmpm */
                int n = SZBYTES[ss];
                EARef se = {2, A(reg), 0, 0}; A(reg) += (reg == 7 && n == 1) ? 2 : n;
                s = ea_read(&se, ss);
                EARef de = {2, A(reg9), 0, 0}; A(reg9) += (reg9 == 7 && n == 1) ? 2 : n;
                d = ea_read(&de, ss);
                do_sub(d, s, ss, 0, 0);
                return;
            }
            ea_get(mode, reg, ss, &e);
            r = ea_read(&e, ss) ^ D(reg9);
            ea_write(&e, ss, r);
            set_logic(r, ss);
            return;
        }
        ea_get(mode, reg, ss, &e);
        s = ea_read(&e, ss);
        do_sub(D(reg9), s, ss, 0, 0);
        return;
    }
    case 0xE: {
        static uint32_t (*const shf[8][3])(uint32_t, uint32_t) = {
            {SH_ASR_B, SH_ASR_W, SH_ASR_L}, {SH_ASL_B, SH_ASL_W, SH_ASL_L},
            {SH_LSR_B, SH_LSR_W, SH_LSR_L}, {SH_LSL_B, SH_LSL_W, SH_LSL_L},
            {SH_ROXR_B, SH_ROXR_W, SH_ROXR_L}, {SH_ROXL_B, SH_ROXL_W, SH_ROXL_L},
            {SH_ROR_B, SH_ROR_W, SH_ROR_L}, {SH_ROL_B, SH_ROL_W, SH_ROL_L},
        };
        int left = (op >> 8) & 1;
        if (ss == 3) {
            int t = (op >> 9) & 7;
            if (t > 3) interp_fatal(op);
            ea_get(mode, reg, SZW, &e);
            d = ea_read(&e, SZW);
            ea_write(&e, SZW, shf[t * 2 + left][SZW](d, 1));
            return;
        }
        int t = (op >> 3) & 3;
        uint32_t cnt = (op & 0x20) ? (D(reg9) & 63) : (reg9 ? reg9 : 8);
        d = D(reg) & SZMASK[ss];
        r = shf[t * 2 + left][ss](d, cnt);
        e.kind = 0; e.reg = reg;
        ea_write(&e, ss, r);
        return;
    }
    default:
        interp_fatal(op);
    }
}

/* execute exactly one instruction (used by the differential tester) */
uint32_t interp_step(uint32_t pc) {
    ipc = pc;
    istop = 0;
    exec_one();
    return ipc;
}

uint32_t interp_run(uint32_t pc) {
    /* re-entrant: syscalls (F$Wait running a child) and calls into recompiled
       code can nest further interpreter runs */
    uint32_t saved_pc = ipc;
    int saved_stop = istop, saved_branch = ibranch;
    ipc = pc;
    istop = 0;
    while (!istop) {
        if (in_static(ipc)) break;
        uint32_t before = ipc;
        ibranch = 0;
        m68k.cyc++;               /* counted before execution, like CYC() in translated code */
        exec_one();
        /* yield at backward branches when the budget is exhausted (same rule as BUDGET) */
        if (ibranch && ipc <= before && m68k.cyc >= m68k.cyc_limit) {
            m68k.yield = 1;
            break;
        }
    }
    uint32_t r = ipc;
    ipc = saved_pc;
    istop = saved_stop;
    ibranch = saved_branch;
    return r;
}

void interp_state_io(SBuf *b) {
    SIO(b, ipc);
    SIO(b, istop);
    SIO(b, ibranch);
}
