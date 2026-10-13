// What fibers do with the FPSCR, VFP exception setting and TPIDRURW of their thread: what a fiber's
// entry sees, what the thread sees after the fiber returns to it, what the fiber sees when the
// thread runs it again, and the same for a switch between two fibers. Each step records what it
// sees and leaves values of its own, and the thread prints at the end.
#include <psp2/fiber.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>

#include "registers.h"
#include "test.h"
#include "threadmgr_extra.h"

#define CONTEXT_SIZE 0x4000
#define STEPS 6

static SceFiber fiber_a __attribute__((aligned(8))), fiber_b __attribute__((aligned(8)));
static char context_a[CONTEXT_SIZE] __attribute__((aligned(8))), context_b[CONTEXT_SIZE] __attribute__((aligned(8)));

static const char *step_names[STEPS] = {
    "the thread before running a fiber", "the fiber at its entry", "the thread after the fiber returned",
    "the fiber run again", "a second fiber switched to", "the first fiber switched back to",
};
static const SceUInt32 step_bits[STEPS] = {
    SCE_KERNEL_VFP_EXCP_QC, SCE_KERNEL_VFP_EXCP_IOC, SCE_KERNEL_VFP_EXCP_DZC,
    SCE_KERNEL_VFP_EXCP_OFC, SCE_KERNEL_VFP_EXCP_UFC, SCE_KERNEL_VFP_EXCP_IXC,
};
static unsigned int seen_fpscr[STEPS], seen_setting[STEPS], seen_tpidrurw[STEPS];

// The FPSCR control bits each step leaves, all different and none the default 0x03000000:
// rounding mode (i % 3) + 1, with flush-to-zero for the first three steps
static unsigned int step_fpscr(int i) {
    return 0x02000000 | (i < 3 ? 0x01000000 : 0) | (unsigned int)((i % 3) + 1) << FPSCR_RMODE_SHIFT;
}

// Records what step sees, then leaves its own FPSCR control bits, VFP exception bit and TPIDRURW
// 0x1000 + step
static void step(int i) {
    seen_fpscr[i] = get_fpscr() & FPSCR_CONTROL;
    seen_setting[i] = sceKernelGetCurrentThreadVfpException();
    seen_tpidrurw[i] = get_tpidrurw();
    // No flags left from earlier operations, for any exception the setting picks
    set_fpscr((get_fpscr() & ~FPSCR_FLAGS & ~FPSCR_CONTROL) | step_fpscr(i));
    // One call can't clear and set the same bit
    sceKernelChangeThreadVfpException(SCE_KERNEL_VFP_EXCP_ALL, 0);
    sceKernelChangeThreadVfpException(0, step_bits[i]);
    set_tpidrurw(0x1000 + i);
}

static void entry_a(SceUInt32 arg_on_initialize, SceUInt32 arg_on_run) {
    (void)arg_on_initialize;
    (void)arg_on_run;
    step(1);
    sceFiberReturnToThread(0, NULL);
    step(3);
    sceFiberSwitch(&fiber_b, 0, NULL);
    step(5);
    for (;;)
        sceFiberReturnToThread(0, NULL);
}

static void entry_b(SceUInt32 arg_on_initialize, SceUInt32 arg_on_run) {
    (void)arg_on_initialize;
    (void)arg_on_run;
    step(4);
    sceFiberSwitch(&fiber_a, 0, NULL);
    for (;;)
        sceFiberReturnToThread(0, NULL);
}

int main(void) {
    for (int i = 0; i < STEPS; i++)
        seen_fpscr[i] = seen_setting[i] = seen_tpidrurw[i] = 0xDEAD;
    // libfiber is a system module an app loads before using it
    PRINT("load the fiber module", sceSysmoduleLoadModule(SCE_SYSMODULE_FIBER));
    PRINT("initialize the first fiber", _sceFiberInitializeImpl(&fiber_a, "a", entry_a, 0, context_a, CONTEXT_SIZE, NULL));
    PRINT("initialize the second fiber", _sceFiberInitializeImpl(&fiber_b, "b", entry_b, 0, context_b, CONTEXT_SIZE, NULL));

    step(0);
    sceFiberRun(&fiber_a, 0, NULL);
    step(2);
    sceFiberRun(&fiber_a, 0, NULL);
    const unsigned int end_fpscr = get_fpscr() & FPSCR_CONTROL;
    const unsigned int end_setting = sceKernelGetCurrentThreadVfpException();
    const unsigned int end_tpidrurw = get_tpidrurw();
    sceKernelChangeThreadVfpException(SCE_KERNEL_VFP_EXCP_ALL, 0);

    for (int i = 1; i < STEPS; i++) {
        PRINTF(seen_fpscr[i], "%s: the FPSCR control bits", step_names[i]);
        PRINTF(seen_setting[i], "%s: the VFP exception setting", step_names[i]);
        PRINTF(seen_tpidrurw[i], "%s: TPIDRURW", step_names[i]);
    }
    PRINT("the thread at the end: the FPSCR control bits", end_fpscr);
    PRINT("the thread at the end: the VFP exception setting", end_setting);
    PRINT("the thread at the end: TPIDRURW", end_tpidrurw);
    PRINT("finalize the first fiber", sceFiberFinalize(&fiber_a));
    PRINT("finalize the second fiber", sceFiberFinalize(&fiber_b));

    test_exit();
    return 0;
}
