// What a callback notified while sceKernelSuspendThreadForVM holds its thread looks like until the
// resume. T waits on a semaphore with callbacks, on core 0 with the VM attribute, and the main
// thread on core 1 suspends it, then notifies T's callback and waits until T's wait type is 0.
// Every case reads what it prints before it prints:
// - one notify: the lowest notify count of the callback over 50 ms while T is suspended, and
//   whether T's pc is still the one from before the notify, then the callback's calls and the
//   count it got after the resume, and the notify count once T waits again
// - two notifies: the lowest notify count over 50 ms while T is suspended, then the callback's
//   calls and the count it got last after the resume
// - one notify, then a cancel of the callback while T is suspended: the notify count after the
//   cancel, then whether the callback ran once T waits again after the resume, and T's status
//
// On a PS Vita TV on firmware 3.65 the kernel takes the notification while T is suspended: the
// notify count drops to 0 and a second notify counts from 1 again, so the callback runs twice after
// the resume, the last time with a count of 1, and a cancel after the first notify doesn't stop it.
// T's pc stays the one from before the notify until the resume.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define T_PRIORITY 0xA0

static SceUID sema, callback;
static volatile int ran, woke, wait_result, cb_calls, cb_count;

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)arg;
    (void)common;
    cb_count = count;
    cb_calls++;
    return 0;
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("cb", 0, callback_entry, NULL);
    ran = 1;
    SceUInt timeout = 3 * TEST_WAIT_US;
    wait_result = sceKernelWaitSemaCB(sema, 1, &timeout);
    woke = 1;
    return 0;
}

// Starts T and waits until it waits with callbacks, with every flag reset
static SceUID start_t(void) {
    ran = woke = cb_calls = 0;
    wait_result = cb_count = 0xDEAD;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID t = sceKernelCreateThread("t", t_entry, T_PRIORITY, 0x4000, THREAD_ATTR_VM, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);
    TEST_WAIT_FOR(ran);
    test_wait_until_waiting(t);
    return t;
}

static void end_t(SceUID t) {
    sceKernelSignalSema(sema, 1);
    test_join_thread(t);
    sceKernelDeleteSema(sema);
}

static SceKernelThreadInfo info_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info;
}

static int notify_count(void) {
    CallbackInfo info = { .size = sizeof(info) };
    const int result = sceKernelGetCallbackInfo(callback, (SceKernelCallbackInfo *)&info);
    return result < 0 ? result : info.notifyCount;
}

// The lowest notify count over 50 ms
static int lowest_notify_count(void) {
    int lowest = notify_count();
    for (int i = 0; i < 50; i++) {
        sceKernelDelayThread(1000);
        const int count = notify_count();
        if (count < lowest)
            lowest = count;
    }
    return lowest;
}

static SceUInt32 pc_of(SceUID thread) {
    ThreadCpuRegisters cpu = { .size = sizeof(cpu) };
    ThreadVfpRegisters vfp = { .size = sizeof(vfp) };
    if (sceKernelGetThreadContextForVM(thread, &cpu, &vfp) < 0)
        return 0;
    return cpu.reg[15];
}

// Notifies T's callback and waits until T's wait ended, which a suspended T shows as wait type 0
static void notify(SceUID t) {
    sceKernelNotifyCallback(callback, 1);
    TEST_WAIT_FOR(info_of(t).waitType == 0);
}

static void one_notify(void) {
    const SceUID t = start_t();
    const int suspended = sceKernelSuspendThreadForVM(t);
    const SceUInt32 pc_before = pc_of(t);
    notify(t);
    const int lowest = lowest_notify_count();
    const int same_pc = pc_before != 0 && pc_of(t) == pc_before;
    const int calls_held = cb_calls;
    const int resumed = sceKernelResumeThreadForVM(t);
    TEST_WAIT_FOR(cb_calls > 0);
    test_wait_until_waiting(t);
    const int calls = cb_calls, count = cb_count, count_after = notify_count();
    end_t(t);
    PRINT("one notify: suspend it", suspended);
    PRINT("one notify: the lowest notify count while suspended", lowest);
    PRINT("one notify: its pc while suspended is the one from before the notify", same_pc);
    PRINT("one notify: the callback's calls while suspended", calls_held);
    PRINT("one notify: resume it", resumed);
    PRINT("one notify: the callback's calls after the resume", calls);
    PRINT("one notify: the count the callback got", count);
    PRINT("one notify: the notify count once it waits again", count_after);
    PRINT("one notify: its wait's result", wait_result);
}

static void two_notifies(void) {
    const SceUID t = start_t();
    const int suspended = sceKernelSuspendThreadForVM(t);
    notify(t);
    sceKernelNotifyCallback(callback, 2);
    const int lowest = lowest_notify_count();
    const int resumed = sceKernelResumeThreadForVM(t);
    TEST_WAIT_FOR(cb_calls > 0);
    test_wait_until_waiting(t);
    const int calls = cb_calls, count = cb_count;
    end_t(t);
    PRINT("two notifies: suspend it", suspended);
    PRINT("two notifies: the lowest notify count while suspended", lowest);
    PRINT("two notifies: resume it", resumed);
    PRINT("two notifies: the callback's calls after the resume", calls);
    PRINT("two notifies: the count the callback got last", count);
    PRINT("two notifies: its wait's result", wait_result);
}

static void cancel_while_suspended(void) {
    const SceUID t = start_t();
    const int suspended = sceKernelSuspendThreadForVM(t);
    notify(t);
    const int canceled = sceKernelCancelCallback(callback);
    const int count = notify_count();
    const int resumed = sceKernelResumeThreadForVM(t);
    // T waits again once it went on after the resume, running the callback first if it does
    test_wait_until_waiting(t);
    const int calls = cb_calls;
    const SceUInt32 status = info_of(t).status;
    end_t(t);
    PRINT("cancel while suspended: suspend it", suspended);
    PRINT("cancel while suspended: cancel the callback", canceled);
    PRINT("cancel while suspended: the notify count after the cancel", count);
    PRINT("cancel while suspended: resume it", resumed);
    PRINT("cancel while suspended: the callback's calls after the resume", calls);
    PRINT("cancel while suspended: its status after the resume", status);
    PRINT("cancel while suspended: its wait's result", wait_result);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    one_notify();
    two_notifies();
    cancel_while_suspended();
    test_exit();
    return 0;
}
