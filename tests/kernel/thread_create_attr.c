// Which attribute bits sceKernelCreateThread accepts, one bit at a time. For each one it accepts,
// the attributes thread info reports, and whether the thread starts with the usual FPSCR and can
// use VFP. Then the same for a thread that sets 0x01000000 on itself.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

// The FPSCR bits that control VFP, without the flags of earlier operations
#define FPSCR_CONTROL 0x07FF9F00

static unsigned int get_fpscr(void) {
    unsigned int value;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(value));
    return value;
}

// Returns whether a VFP add gives the right sum
static int vfp_works(void) {
    volatile float a = 1.5f, b = 2.25f;
    volatile float sum = a + b;
    return sum == 3.75f;
}

static volatile unsigned int seen_fpscr, seen_works;

static int report_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    seen_fpscr = get_fpscr() & FPSCR_CONTROL;
    seen_works = vfp_works();
    return 0;
}

static volatile int set_result, clear_result;
static volatile unsigned int set_fpscr, set_works;

static int set_attr_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    set_result = sceKernelChangeCurrentThreadAttr(0, 0x01000000);
    set_fpscr = get_fpscr() & FPSCR_CONTROL;
    set_works = vfp_works();
    clear_result = sceKernelChangeCurrentThreadAttr(0x01000000, 0);
    return 0;
}

int main(void) {
    for (int bit = 0; bit < 32; bit++) {
        const SceUInt32 attr = 1u << bit;
        const SceUID thread = sceKernelCreateThread("attr", report_thread, 0x10000100, 0x4000, attr, 0, NULL);
        PRINTF(test_created(thread), "create with 0x%08X", (unsigned)attr);
        if (thread < 0)
            continue;
        SceKernelThreadInfo info = { .size = sizeof(info) };
        sceKernelGetThreadInfo(thread, &info);
        PRINTF(info.attr, "create with 0x%08X: the attributes in its info", (unsigned)attr);
        seen_fpscr = seen_works = 0xDEAD;
        sceKernelStartThread(thread, 0, NULL);
        test_join_thread(thread);
        PRINTF(seen_fpscr, "create with 0x%08X: its FPSCR control bits", (unsigned)attr);
        PRINTF(seen_works, "create with 0x%08X: VFP works", (unsigned)attr);
    }

    test_join_thread(test_start_thread(set_attr_thread));
    PRINT("a thread sets 0x01000000 on itself", set_result);
    PRINT("then its FPSCR control bits", set_fpscr);
    PRINT("then VFP works", set_works);
    PRINT("it clears 0x01000000", clear_result);

    test_exit();
    return 0;
}
