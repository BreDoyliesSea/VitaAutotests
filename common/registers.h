// Reads and writes the VFP control register FPSCR and the thread ID registers TPIDRURW and TPIDRURO
#pragma once

// The FPSCR bits that control VFP, without the flags of earlier operations
#define FPSCR_CONTROL 0x07FF9F00
// The condition flags and cumulative exception flags of FPSCR
#define FPSCR_FLAGS 0xF800009F
#define FPSCR_RMODE_SHIFT 22
#define RMODE_NEAREST 0
#define RMODE_PLUS 1
#define RMODE_MINUS 2
#define RMODE_ZERO 3

static inline unsigned int get_fpscr(void) {
    unsigned int value;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(value));
    return value;
}

static inline void set_fpscr(unsigned int value) {
    __asm__ volatile("vmsr fpscr, %0" : : "r"(value));
}

static inline int get_rmode(void) {
    return (get_fpscr() >> FPSCR_RMODE_SHIFT) & 3;
}

static inline void set_rmode(int mode) {
    set_fpscr((get_fpscr() & ~(3u << FPSCR_RMODE_SHIFT)) | ((unsigned int)mode << FPSCR_RMODE_SHIFT));
}

static inline unsigned int get_tpidrurw(void) {
    unsigned int value;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(value));
    return value;
}

static inline void set_tpidrurw(unsigned int value) {
    __asm__ volatile("mcr p15, 0, %0, c13, c0, 2" : : "r"(value));
}

static inline unsigned int get_tpidruro(void) {
    unsigned int value;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return value;
}

// Loads d8 and d16 from in, calls fn(a0, a1, a2), and stores d8 and d16 to out if out isn't NULL,
// in one asm block so no compiler code touches the two registers in between
static inline int call_with_d8_d16(int (*fn)(int, int, void *), int a0, int a1, void *a2, const unsigned long long *in, unsigned long long *out) {
    register int r0 __asm__("r0") = a0;
    register int r1 __asm__("r1") = a1;
    register void *r2 __asm__("r2") = a2;
    __asm__ volatile(
        "vldr d8, [%[in]]\n\t"
        "vldr d16, [%[in], #8]\n\t"
        "blx %[fn]\n\t"
        "cmp %[out], #0\n\t"
        "beq 1f\n\t"
        "vstr d8, [%[out]]\n\t"
        "vstr d16, [%[out], #8]\n\t"
        "1:\n\t"
        : "+r"(r0), "+r"(r1), "+r"(r2)
        : [in] "r"(in), [out] "r"(out), [fn] "r"(fn)
        : "r3", "r12", "lr", "memory", "cc", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8",
          "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31");
    return r0;
}
