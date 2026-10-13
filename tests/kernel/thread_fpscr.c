// The VFP control register FPSCR of each thread: what a new thread starts with, whether its
// rounding mode survives a delay and another thread running on its core, what a callback sees and
// whether a change made in it stays, and what a restart keeps
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// The FPSCR bits that control VFP, without the flags of earlier operations
#define FPSCR_CONTROL 0x07FF9F00
#define FPSCR_RMODE_SHIFT 22
#define RMODE_PLUS 1
#define RMODE_MINUS 2
#define RMODE_ZERO 3

static unsigned int get_fpscr(void) {
    unsigned int value;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(value));
    return value;
}

static void set_fpscr(unsigned int value) {
    __asm__ volatile("vmsr fpscr, %0" : : "r"(value));
}

static int get_rmode(void) {
    return (get_fpscr() >> FPSCR_RMODE_SHIFT) & 3;
}

static void set_rmode(int mode) {
    set_fpscr((get_fpscr() & ~(3u << FPSCR_RMODE_SHIFT)) | ((unsigned int)mode << FPSCR_RMODE_SHIFT));
}

static SceUID sema, cb;
static volatile int phase, runs;
static volatile unsigned int new_fpscr, after_delay, after_switch, cb_rmode, after_cb, check_cb_rmode, after_check, restart_rmode;

// Sees the thread's rounding mode and changes it
static int rmode_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    if (phase == 0)
        cb_rmode = get_rmode();
    else
        check_cb_rmode = get_rmode();
    set_rmode(RMODE_ZERO);
    return 0;
}

static int fpscr_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    runs++;
    if (runs == 2) {
        restart_rmode = get_rmode();
        return 0;
    }
    new_fpscr = get_fpscr() & FPSCR_CONTROL;

    set_rmode(RMODE_PLUS);
    sceKernelDelayThread(1000);
    after_delay = get_rmode();

    // Another thread runs on this core and sets its own rounding mode while this one waits
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    after_switch = get_rmode();

    // A callback run by a wait
    set_rmode(RMODE_PLUS);
    cb = sceKernelCreateCallback("rmode", 0, rmode_callback, NULL);
    sceKernelNotifyCallback(cb, 1);
    timeout = 1000;
    sceKernelWaitSemaCB(sema, 1, &timeout);
    after_cb = get_rmode();

    // A callback run by a check
    phase = 1;
    set_rmode(RMODE_PLUS);
    sceKernelNotifyCallback(cb, 1);
    sceKernelCheckCallback();
    after_check = get_rmode();
    sceKernelDeleteCallback(cb);

    // What the next run starts with
    set_rmode(RMODE_MINUS);
    return 0;
}

static int other_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    set_rmode(RMODE_MINUS);
    sceKernelSignalSema(sema, 1);
    return 0;
}

int main(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    PRINT("the main thread's FPSCR control bits", get_fpscr() & FPSCR_CONTROL);

    // Both threads on core 1, so the other one runs on the core the first one waits on
    const SceUID thread = sceKernelCreateThread("fpscr", fpscr_thread, 0x10000100, 0x4000, 0, CPU_MASK_USER_1, NULL);
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema));
    const SceUID other = sceKernelCreateThread("other", other_thread, 0x10000100, 0x4000, 0, CPU_MASK_USER_1, NULL);
    sceKernelStartThread(other, 0, NULL);
    test_join_thread(other);
    sceKernelWaitThreadEnd(thread, NULL, NULL);

    PRINT("a new thread's FPSCR control bits", new_fpscr);
    PRINT("after a delay: the rounding mode", after_delay);
    PRINT("after another thread ran on its core: the rounding mode", after_switch);
    PRINT("a callback run by a wait sees the rounding mode", cb_rmode);
    PRINT("after the wait: the rounding mode", after_cb);
    PRINT("a callback run by a check sees the rounding mode", check_cb_rmode);
    PRINT("after the check: the rounding mode", after_check);

    sceKernelStartThread(thread, 0, NULL);
    test_join_thread(thread);
    PRINT("started again: the rounding mode", restart_rmode);

    test_exit();
    return 0;
}
