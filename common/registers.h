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
