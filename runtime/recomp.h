/* Interface between recompiled guest code and the runtime. Must stay C. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union { double d; uint64_t u; } FPR;

typedef struct CPU {
    uint32_t r[32];
    FPR f[32];         /* ps0 */
    double ps1[32];
    uint8_t cr[8];     /* each field: LT=8 GT=4 EQ=2 SO=1 */
    uint8_t xer_ca, xer_so, xer_ov, xer_bc;
    uint32_t lr, ctr, msr, fpscr;
    uint32_t gqr[8];
    uint32_t spr[1024];
    void* host;        /* owning HostThread */
    uint32_t trace[256];  /* ring buffer of recently entered functions */
    uint32_t trace_pos;
} CPU;

#ifdef WR_WATCH
void debug_watch_check(CPU* c, uint32_t fn);
#define ENTER(addr) (c->trace[c->trace_pos++ & 255] = (addr), debug_watch_check(c, (addr)))
#define WATCH_STORE(pc) debug_watch_check(c, (pc) | 1u)
#else
#define WATCH_STORE(pc) ((void)0)
#define ENTER(addr) (c->trace[c->trace_pos++ & 255] = (addr))
#endif

#define MSR_EE 0x8000u

/* ---- memory ---- */
extern uint8_t* g_mem;             /* 1GB host reservation; guest addr & 0x3FFFFFFF */
#define HOST(a) (g_mem + ((uint32_t)(a) & 0x3FFFFFFFu))
#define IS_MMIO(a) ((uint32_t)((a) - 0xC8000000u) < 0x08000000u)

uint32_t mmio_read32(uint32_t a);
uint16_t mmio_read16(uint32_t a);
uint8_t mmio_read8(uint32_t a);
void mmio_write32(uint32_t a, uint32_t v);
void mmio_write16(uint32_t a, uint16_t v);
void mmio_write8(uint32_t a, uint8_t v);

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

static inline uint32_t LD32(uint32_t a) {
    if (UNLIKELY(IS_MMIO(a))) return mmio_read32(a);
    uint32_t v; memcpy(&v, HOST(a), 4); return __builtin_bswap32(v);
}
static inline uint32_t LD16(uint32_t a) {
    if (UNLIKELY(IS_MMIO(a))) return mmio_read16(a);
    uint16_t v; memcpy(&v, HOST(a), 2); return __builtin_bswap16(v);
}
static inline uint32_t LDS16(uint32_t a) { return (uint32_t)(int32_t)(int16_t)LD16(a); }
static inline uint32_t LD8(uint32_t a) {
    if (UNLIKELY(IS_MMIO(a))) return mmio_read8(a);
    return *HOST(a);
}
static inline uint32_t LD32BR(uint32_t a) { return __builtin_bswap32(LD32(a)); }
static inline uint32_t LD16BR(uint32_t a) { return __builtin_bswap16((uint16_t)LD16(a)); }
static inline uint64_t LD64(uint32_t a) {
    if (UNLIKELY(IS_MMIO(a))) return ((uint64_t)mmio_read32(a) << 32) | mmio_read32(a + 4);
    uint64_t v; memcpy(&v, HOST(a), 8); return __builtin_bswap64(v);
}
static inline void ST32(uint32_t a, uint32_t v) {
    if (UNLIKELY(IS_MMIO(a))) { mmio_write32(a, v); return; }
    v = __builtin_bswap32(v); memcpy(HOST(a), &v, 4);
}
static inline void ST16(uint32_t a, uint32_t v) {
    if (UNLIKELY(IS_MMIO(a))) { mmio_write16(a, (uint16_t)v); return; }
    uint16_t h = __builtin_bswap16((uint16_t)v); memcpy(HOST(a), &h, 2);
}
static inline void ST8(uint32_t a, uint32_t v) {
    if (UNLIKELY(IS_MMIO(a))) { mmio_write8(a, (uint8_t)v); return; }
    *HOST(a) = (uint8_t)v;
}
static inline void ST32BR(uint32_t a, uint32_t v) { ST32(a, __builtin_bswap32(v)); }
static inline void ST16BR(uint32_t a, uint32_t v) { ST16(a, __builtin_bswap16((uint16_t)v)); }
static inline void ST64(uint32_t a, uint64_t v) {
    if (UNLIKELY(IS_MMIO(a))) { mmio_write32(a, (uint32_t)(v >> 32)); mmio_write32(a + 4, (uint32_t)v); return; }
    v = __builtin_bswap64(v); memcpy(HOST(a), &v, 8);
}
static inline double LDF32(uint32_t a) {
    uint32_t v = LD32(a); float f; memcpy(&f, &v, 4); return (double)f;
}
static inline void STF32(uint32_t a, double d) {
    float f = (float)d; uint32_t v; memcpy(&v, &f, 4); ST32(a, v);
}

/* ---- condition register / xer ---- */
#define CRB(n) ((c->cr[(n) >> 2] >> (3 - ((n) & 3))) & 1)
#define SET_CRB(n, v) do { int _s = 3 - ((n) & 3); \
    c->cr[(n) >> 2] = (uint8_t)((c->cr[(n) >> 2] & ~(1 << _s)) | ((!!(v)) << _s)); } while (0)
#define CMPS(a, b) ((uint8_t)(((a) < (b) ? 8 : (a) > (b) ? 4 : 2) | c->xer_so))
#define CMPU(a, b) ((uint8_t)(((uint32_t)(a) < (uint32_t)(b) ? 8 : (uint32_t)(a) > (uint32_t)(b) ? 4 : 2) | c->xer_so))
#define SET_CR0(v) (c->cr[0] = CMPS((int32_t)(v), 0))
#define SET_OV(x) do { c->xer_ov = (uint8_t)(x); c->xer_so |= c->xer_ov; } while (0)
#define ROTL(x, n) (((uint32_t)(x) << (n)) | ((uint32_t)(x) >> ((32 - (n)) & 31)))

static inline uint32_t cpu_get_cr(const CPU* c) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint32_t)c->cr[i] << (28 - 4 * i);
    return v;
}
static inline void cpu_set_cr(CPU* c, uint32_t v) {
    for (int i = 0; i < 8; i++) c->cr[i] = (v >> (28 - 4 * i)) & 0xF;
}
#define GET_CR() cpu_get_cr(c)
#define SET_CR(v) cpu_set_cr(c, (v))
#define GET_XER() (((uint32_t)c->xer_so << 31) | ((uint32_t)c->xer_ov << 30) | ((uint32_t)c->xer_ca << 29) | c->xer_bc)
#define SET_XER(v) do { uint32_t _v = (v); c->xer_so = _v >> 31; c->xer_ov = (_v >> 30) & 1; \
    c->xer_ca = (_v >> 29) & 1; c->xer_bc = _v & 0x7F; } while (0)

static inline uint8_t fcmp(double a, double b) {
    if (a < b) return 8;
    if (a > b) return 4;
    if (a == b) return 2;
    return 1;
}
#define FCMP(a, b) fcmp((a), (b))

static inline int32_t FCTIWZ(double d) {
    if (d != d) return INT32_MIN;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (int32_t)d;
}
static inline int32_t FCTIW(double d) {
    if (d != d) return INT32_MIN;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (int32_t)nearbyint(d);
}

/* ---- runtime services called from generated code ---- */
typedef void (*RecompFn)(CPU*);
typedef struct { uint32_t addr; RecompFn fn; const char* name; } RecompFunc;
extern const RecompFunc g_recomp_funcs[];
extern const uint32_t g_recomp_func_count;

void call_indirect(CPU* c, uint32_t addr);
void unimpl(CPU* c, uint32_t pc, uint32_t inst);
void hle_sc(CPU* c, uint32_t pc);
void hle_trap(CPU* c, uint32_t pc, uint32_t inst);
void hle_rfi(CPU* c, uint32_t pc);
void hle_mtmsr(CPU* c, uint32_t v);
uint32_t hle_mfspr(CPU* c, uint32_t spr);
void hle_mtspr(CPU* c, uint32_t spr, uint32_t v);
void hle_set_fpscr(CPU* c, uint32_t v);
void hle_dcbz(CPU* c, uint32_t ea);
void hle_dcbz_l(CPU* c, uint32_t ea);
void hle_dcbi(CPU* c, uint32_t ea);
void hle_lswi(CPU* c, uint32_t ea, int rd, int n);
void hle_stswi(CPU* c, uint32_t ea, int rs, int n);
void psq_load(CPU* c, uint32_t ea, int frd, int w, int gqr);
void psq_store(CPU* c, uint32_t ea, int frs, int w, int gqr);
jmp_buf* hle_context_jmpbuf(CPU* c);

extern volatile int g_irq_pending;
void irq_poll(CPU* c);
#define IRQ_CHECK() do { if (UNLIKELY(g_irq_pending)) irq_poll(c); } while (0)

#ifdef __cplusplus
}
#endif
