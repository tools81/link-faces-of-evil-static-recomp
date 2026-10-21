/* Core definitions shared by the generated code and the runtime. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef _MSC_VER
#define FORCEINLINE __forceinline
#else
#define FORCEINLINE inline __attribute__((always_inline))
#endif

/* ---------------------------------------------------------------- CPU state */
typedef struct {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t pc;
    uint8_t xf, nf, zf, vf, cf;
    uint8_t srh;            /* system byte of SR (T, S, I2-I0) */
    int64_t cyc;            /* instruction counter */
    int64_t cyc_limit;      /* when cyc >= cyc_limit, translated code yields */
    int yield;              /* set when code yielded at a backward branch */
} M68K;

extern M68K m68k;

#define D(n) m68k.d[n]
#define A(n) m68k.a[n]
#define XF m68k.xf
#define NF m68k.nf
#define ZF m68k.zf
#define VF m68k.vf
#define CF m68k.cf

#define CYC(n) (m68k.cyc += (n))
#define BUDGET(pc_) do { if (m68k.cyc >= m68k.cyc_limit) { m68k.yield = 1; return (pc_); } } while (0)

static FORCEINLINE uint8_t GET_CCR(void) {
    return (uint8_t)((XF << 4) | (NF << 3) | (ZF << 2) | (VF << 1) | CF);
}
static FORCEINLINE void SET_CCR(uint32_t v) {
    XF = (v >> 4) & 1; NF = (v >> 3) & 1; ZF = (v >> 2) & 1; VF = (v >> 1) & 1; CF = v & 1;
}
static FORCEINLINE uint16_t GET_SR(void) { return (uint16_t)((m68k.srh << 8) | GET_CCR()); }
#define SET_SRH(v) (m68k.srh = (uint8_t)((v) & 0xA7))

/* ---------------------------------------------------------------- memory */
#define MEM_SIZE  0x01000000u   /* 16 MB flat address space */
#define MEM_MASK  0x00FFFFFFu
extern uint8_t *RAM;

void mem_fault(uint32_t addr, int write);

static FORCEINLINE uint8_t RB(uint32_t a) { return RAM[a & MEM_MASK]; }
static FORCEINLINE uint16_t RW(uint32_t a) {
    a &= MEM_MASK;
    return (uint16_t)((RAM[a] << 8) | RAM[a + 1]);
}
static FORCEINLINE uint32_t RL(uint32_t a) {
    a &= MEM_MASK;
    return ((uint32_t)RAM[a] << 24) | ((uint32_t)RAM[a + 1] << 16) | ((uint32_t)RAM[a + 2] << 8) | RAM[a + 3];
}
#ifdef RECOMP_TEST
void test_log_write(uint32_t a, int n);
#define TEST_LOG(a, n) test_log_write(a, n)
#else
#define TEST_LOG(a, n) ((void)0)
#endif
static FORCEINLINE void WB(uint32_t a, uint32_t v) { TEST_LOG(a, 1); RAM[a & MEM_MASK] = (uint8_t)v; }
static FORCEINLINE void WW(uint32_t a, uint32_t v) {
    TEST_LOG(a, 2);
    a &= MEM_MASK;
    RAM[a] = (uint8_t)(v >> 8); RAM[a + 1] = (uint8_t)v;
}
static FORCEINLINE void WL(uint32_t a, uint32_t v) {
    TEST_LOG(a, 4);
    a &= MEM_MASK;
    RAM[a] = (uint8_t)(v >> 24); RAM[a + 1] = (uint8_t)(v >> 16);
    RAM[a + 2] = (uint8_t)(v >> 8); RAM[a + 3] = (uint8_t)v;
}

/* ---------------------------------------------------------------- helpers */
typedef uint32_t (*RecompFn)(uint32_t pc);
typedef struct { uint32_t addr; RecompFn fn; } RecompEntry;
typedef struct { uint32_t addr; const char *name; } RecompName;

extern const RecompEntry recomp_entries[];
extern const int recomp_num_entries;
extern const RecompName recomp_names[];
extern const int recomp_num_names;
extern const uint32_t recomp_code_lo, recomp_code_hi;

uint32_t recomp_bad_entry(uint32_t pc, uint32_t func);
uint32_t m68k_call(uint32_t target);
uint32_t m68k_exception(int vec, uint32_t addr);
uint32_t m68k_trap(int n, uint32_t func, uint32_t next);
uint32_t os9_syscall(uint32_t code, uint32_t next);
void m68k_divu(int reg, uint32_t src, uint32_t addr);
void m68k_divs(int reg, uint32_t src, uint32_t addr);
void a5_check(uint32_t addr);
extern uint32_t a5_expected;
#define A5CHECK(addr) do { if (A(5) != a5_expected) a5_check(addr); } while (0)

uint8_t BCD_ADD(uint8_t d, uint8_t s);
uint8_t BCD_SUB(uint8_t d, uint8_t s);

/* shifts / rotates: set all flags as the 68000 does */
#define DECL_SH(OP) \
    uint32_t SH_##OP##_B(uint32_t d, uint32_t n); \
    uint32_t SH_##OP##_W(uint32_t d, uint32_t n); \
    uint32_t SH_##OP##_L(uint32_t d, uint32_t n);
DECL_SH(ASL) DECL_SH(ASR) DECL_SH(LSL) DECL_SH(LSR)
DECL_SH(ROL) DECL_SH(ROR) DECL_SH(ROXL) DECL_SH(ROXR)

/* runtime hooks on game functions (see HOOKS in tools/gen.py) */
uint32_t hook_hero_hit(void);
uint32_t hook_kill_sprite(void);

#include "recomp_funcs.h"
