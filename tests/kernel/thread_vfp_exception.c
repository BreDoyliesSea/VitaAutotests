// The VFP exception setting of a thread, which sceKernelChangeThreadVfpException changes: what a
// thread starts with, which bits the call takes and what it returns, whether FPSCR shows the
// setting, what a thread it creates and its callbacks see, and what a restart keeps. The thread
// records everything and the main thread prints it after the thread ends.
#include <psp2/kernel/threadmgr.h>

#include "registers.h"
#include "test.h"
#include "threadmgr_extra.h"

// Each bit the header defines, and some it doesn't
static const SceUInt32 bits[] = {
    SCE_KERNEL_VFP_EXCP_IOC, SCE_KERNEL_VFP_EXCP_DZC, SCE_KERNEL_VFP_EXCP_OFC, SCE_KERNEL_VFP_EXCP_UFC,
    SCE_KERNEL_VFP_EXCP_IXC, 0x20, 0x40, SCE_KERNEL_VFP_EXCP_IDC, 0x100, SCE_KERNEL_VFP_EXCP_QC,
    0x80000000,
};
#define BITS (sizeof(bits) / sizeof(bits[0]))

static volatile int set_result[BITS], clear_result[BITS];
static volatile unsigned int after_set[BITS], fpscr_after_set[BITS], after_clear[BITS], fpscr_after_clear[BITS];
static volatile int all_result, none_result, cb_change, runs;
static volatile unsigned int initial, all_setting, all_fpscr, child_setting, child_fpscr;
static volatile unsigned int check_cb_setting, check_cb_fpscr, after_check, delay_cb_setting, after_delay, none_setting;
static volatile unsigned int restart_setting;
static SceUID cb;
static volatile int phase;

static int report_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    child_setting = sceKernelGetCurrentThreadVfpException();
    child_fpscr = get_fpscr() & FPSCR_CONTROL;
    return 0;
}

static int setting_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    if (phase == 0) {
        check_cb_setting = sceKernelGetCurrentThreadVfpException();
        check_cb_fpscr = get_fpscr() & FPSCR_CONTROL;
    } else {
        delay_cb_setting = sceKernelGetCurrentThreadVfpException();
        // Returns the setting before the change
        cb_change = sceKernelChangeThreadVfpException(0, SCE_KERNEL_VFP_EXCP_IOC);
    }
    return 0;
}

static int setting_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    runs++;
    if (runs == 2) {
        restart_setting = sceKernelGetCurrentThreadVfpException();
        sceKernelChangeThreadVfpException(SCE_KERNEL_VFP_EXCP_ALL, 0);
        return 0;
    }
    // No flags left from earlier operations, for any exception the setting picks
    set_fpscr(get_fpscr() & ~FPSCR_FLAGS);
    initial = sceKernelGetCurrentThreadVfpException();
    for (unsigned int i = 0; i < BITS; i++) {
        set_result[i] = sceKernelChangeThreadVfpException(0, bits[i]);
        after_set[i] = sceKernelGetCurrentThreadVfpException();
        fpscr_after_set[i] = get_fpscr() & FPSCR_CONTROL;
        clear_result[i] = sceKernelChangeThreadVfpException(bits[i], 0);
        after_clear[i] = sceKernelGetCurrentThreadVfpException();
        fpscr_after_clear[i] = get_fpscr() & FPSCR_CONTROL;
    }

    all_result = sceKernelChangeThreadVfpException(0, SCE_KERNEL_VFP_EXCP_ALL);
    all_setting = sceKernelGetCurrentThreadVfpException();
    all_fpscr = get_fpscr() & FPSCR_CONTROL;
    test_join_thread(test_start_thread(report_thread));

    cb = sceKernelCreateCallback("setting", 0, setting_callback, NULL);
    sceKernelNotifyCallback(cb, 1);
    sceKernelCheckCallback();
    after_check = sceKernelGetCurrentThreadVfpException();
    phase = 1;
    sceKernelNotifyCallback(cb, 1);
    sceKernelDelayThreadCB(1000);
    after_delay = sceKernelGetCurrentThreadVfpException();
    sceKernelDeleteCallback(cb);

    none_result = sceKernelChangeThreadVfpException(SCE_KERNEL_VFP_EXCP_ALL, 0);
    none_setting = sceKernelGetCurrentThreadVfpException();

    // What the next run starts with
    sceKernelChangeThreadVfpException(0, SCE_KERNEL_VFP_EXCP_DZC);
    return 0;
}

int main(void) {
    PRINT("the main thread's setting", sceKernelGetCurrentThreadVfpException());

    const SceUID thread = sceKernelCreateThread("setting", setting_thread, 0x10000100, 0x4000, 0, 0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    sceKernelWaitThreadEnd(thread, NULL, NULL);

    PRINT("a new thread's setting", initial);
    for (unsigned int i = 0; i < BITS; i++) {
        PRINTF(set_result[i], "set 0x%08X", (unsigned)bits[i]);
        PRINTF(after_set[i], "set 0x%08X: the setting after", (unsigned)bits[i]);
        PRINTF(fpscr_after_set[i], "set 0x%08X: the FPSCR control bits after", (unsigned)bits[i]);
        PRINTF(clear_result[i], "clear 0x%08X", (unsigned)bits[i]);
        PRINTF(after_clear[i], "clear 0x%08X: the setting after", (unsigned)bits[i]);
        PRINTF(fpscr_after_clear[i], "clear 0x%08X: the FPSCR control bits after", (unsigned)bits[i]);
    }
    PRINT("set every bit", all_result);
    PRINT("set every bit: the setting after", all_setting);
    PRINT("set every bit: the FPSCR control bits after", all_fpscr);
    PRINT("a thread it creates: the setting", child_setting);
    PRINT("a thread it creates: the FPSCR control bits", child_fpscr);
    PRINT("a callback run by a check: the setting", check_cb_setting);
    PRINT("a callback run by a check: the FPSCR control bits", check_cb_fpscr);
    PRINT("after the check: the setting", after_check);
    PRINT("a callback run by a delay: the setting", delay_cb_setting);
    PRINT("it sets 0x00000001, which returns", cb_change);
    PRINT("after the delay: the setting", after_delay);
    PRINT("clear every bit", none_result);
    PRINT("clear every bit: the setting after", none_setting);

    sceKernelStartThread(thread, 0, NULL);
    test_join_thread(thread);
    PRINT("started again after setting 0x00000002: the setting", restart_setting);

    test_exit();
    return 0;
}
