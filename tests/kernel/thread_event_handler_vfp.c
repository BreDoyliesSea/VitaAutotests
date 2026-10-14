// What a thread event handler starts with: the FPSCR, VFP exception setting and TPIDRURW that the
// handlers for a thread's start and end and the thread's entry see, when each step leaves values of
// its own, and the same for the thread started again. Every step records, and the main thread
// prints once the target ended.
#include <psp2/kernel/threadmgr.h>

#include "registers.h"
#include "test.h"
#include "threadmgr_extra.h"

#define THREAD_EVENT_START 0x4
#define THREAD_EVENT_EXIT 0x8

#define STEPS 3
static const char *step_names[STEPS] = { "the start handler", "the entry", "the end handler" };
// The VFP exception setting each step leaves
static const SceUInt32 step_bits[STEPS] = { SCE_KERNEL_VFP_EXCP_IOC, SCE_KERNEL_VFP_EXCP_DZC, SCE_KERNEL_VFP_EXCP_OFC };

typedef struct Seen {
    unsigned int fpscr, setting, tpidrurw, on_target;
} Seen;

static Seen seen[2][STEPS];
static volatile int run;
static SceUID target;

static void record(int step) {
    Seen *s = &seen[run][step];
    s->fpscr = get_fpscr() & FPSCR_CONTROL;
    s->setting = sceKernelGetCurrentThreadVfpException();
    s->tpidrurw = get_tpidrurw();
    s->on_target = sceKernelGetThreadId() == target;
}

// Leaves rounding mode step + 1, the step's VFP exception bit and TPIDRURW 0x1000 + step
static void change(int step) {
    // No flags left from earlier operations, for any exception the setting picks
    set_fpscr(get_fpscr() & ~FPSCR_FLAGS);
    set_rmode(step + 1);
    // One call can't clear and set the same bit
    sceKernelChangeThreadVfpException(SCE_KERNEL_VFP_EXCP_ALL, 0);
    sceKernelChangeThreadVfpException(0, step_bits[step]);
    set_tpidrurw(0x1000 + step);
}

static int handler(SceInt32 type, SceUID thread_id, SceInt32 arg, void *common) {
    (void)thread_id;
    (void)arg;
    (void)common;
    const int step = type == THREAD_EVENT_START ? 0 : 2;
    record(step);
    change(step);
    return 0;
}

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(1);
    change(1);
    return 0;
}

int main(void) {
    for (int r = 0; r < 2; r++) {
        for (int step = 0; step < STEPS; step++)
            seen[r][step] = (Seen){ 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD };
    }
    // Values of the main thread's own, which the target shouldn't see
    set_rmode(RMODE_ZERO);
    sceKernelChangeThreadVfpException(0, SCE_KERNEL_VFP_EXCP_QC);
    set_tpidrurw(0x7777);

    target = sceKernelCreateThread("target", entry, 0x10000100, 0x4000, 0, 0, NULL);
    const SceUID id = sceKernelRegisterThreadEventHandler("vfp", target, THREAD_EVENT_START | THREAD_EVENT_EXIT, handler, NULL);
    PRINT("register the handler", test_created(id));
    run = 0;
    sceKernelStartThread(target, 0, NULL);
    sceKernelWaitThreadEnd(target, NULL, NULL);
    run = 1;
    sceKernelStartThread(target, 0, NULL);
    sceKernelWaitThreadEnd(target, NULL, NULL);
    if (id >= 0)
        sceKernelUnregisterThreadEventHandler(id);
    sceKernelDeleteThread(target);

    for (int r = 0; r < 2; r++) {
        const char *run_name = r == 0 ? "first run" : "started again";
        for (int step = 0; step < STEPS; step++) {
            const Seen *s = &seen[r][step];
            PRINTF(s->fpscr, "%s: %s: the FPSCR control bits", run_name, step_names[step]);
            PRINTF(s->setting, "%s: %s: the VFP exception setting", run_name, step_names[step]);
            PRINTF(s->tpidrurw, "%s: %s: TPIDRURW", run_name, step_names[step]);
            PRINTF(s->on_target, "%s: %s: runs on the target", run_name, step_names[step]);
        }
    }
    PRINT("the main thread's rounding mode after", get_rmode());
    PRINT("the main thread's VFP exception setting after", sceKernelGetCurrentThreadVfpException());
    PRINT("the main thread's TPIDRURW after", get_tpidrurw());

    test_exit();
    return 0;
}
