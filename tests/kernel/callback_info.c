// What a thread reports about itself and its callbacks while one of them runs, seen from inside
// the callback and from another core, and the arguments a callback gets
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID sema, cb, notifier;
static int common_marker;
static volatile int got_notify_id, got_count, got_arg, got_common, got_status, got_wait_type, got_wait_id, got_waiters;
static volatile int got_pending, got_info_result, got_info_count, got_info_arg;

static int recording_callback(int notify_id, int count, int arg, void *common) {
    got_notify_id = notify_id;
    got_count = count;
    got_arg = arg;
    got_common = common == &common_marker;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
    got_status = info.status;
    got_wait_type = info.waitType;
    got_wait_id = info.waitId == sema;
    SceKernelSemaInfo sema_info = { .size = sizeof(sema_info) };
    sceKernelGetSemaInfo(sema, &sema_info);
    got_waiters = sema_info.numWaitThreads;
    got_pending = sceKernelGetCallbackCount(cb);
    CallbackInfo cb_info = { .size = sizeof(cb_info) };
    got_info_result = sceKernelGetCallbackInfo(cb, (SceKernelCallbackInfo *)&cb_info);
    got_info_count = cb_info.notifyCount;
    got_info_arg = cb_info.notifyArg;
    return 0;
}

static void print_recorded(const char *name) {
    PRINTF(got_notify_id == -1, "%s: the notify ID is -1", name);
    PRINTF(got_notify_id == 0, "%s: the notify ID is 0", name);
    PRINTF(got_notify_id == cb, "%s: the notify ID is the callback", name);
    PRINTF(got_notify_id == notifier, "%s: the notify ID is the notifying thread", name);
    PRINTF(got_count, "%s: the count", name);
    PRINTF(got_arg, "%s: the argument", name);
    PRINTF(got_common, "%s: the common pointer is the one given at create", name);
    PRINTF(got_status, "%s: the thread's status inside", name);
    PRINTF(got_wait_type, "%s: its wait type inside", name);
    PRINTF(got_wait_id, "%s: its wait UID inside is the semaphore", name);
    PRINTF(got_waiters, "%s: the semaphore's waiters inside", name);
    PRINTF(got_pending, "%s: the callback's notifications inside", name);
    PRINTF(got_info_result, "%s: the callback's info inside", name);
    PRINTF(got_info_count, "%s: the notification count in its info inside", name);
    PRINTF(got_info_arg, "%s: the argument in its info inside", name);
}

static int notify_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelNotifyCallback(cb, 0x77);
    return 0;
}

// A thread on another core whose callback spins until released, so what the test reads while it
// spins can't change
static SceUID busy_sema, busy_cb;
static volatile int in_callback, release, busy_result;

static int busy_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    in_callback = 1;
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    while (!release && sceKernelGetProcessTimeWide() - start < 5 * TEST_WAIT_US)
        ;
    return 0;
}

static int busy_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    busy_cb = sceKernelCreateCallback("busy", 0, busy_callback, NULL);
    SceUInt timeout = 2 * TEST_WAIT_US;
    busy_result = sceKernelWaitSemaCB(busy_sema, 1, &timeout);
    sceKernelDeleteCallback(busy_cb);
    return 0;
}

static SceUID plain_sema, plain_cb;

static int plain_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    plain_cb = sceKernelCreateCallback("plain", 0, recording_callback, NULL);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(plain_sema, 1, &timeout);
    sceKernelDeleteCallback(plain_cb);
    return 0;
}

int main(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    cb = sceKernelCreateCallback("record", 0, recording_callback, &common_marker);
    notifier = sceKernelGetThreadId();

    // Run by a check
    sceKernelNotifyCallback(cb, 0x11);
    sceKernelNotifyCallback(cb, 0x12);
    sceKernelCheckCallback();
    print_recorded("check");

    // Run by a delay with callbacks
    sceKernelNotifyCallback(cb, 0x21);
    sceKernelDelayThreadCB(1000);
    print_recorded("delay");

    // Run by a semaphore wait with callbacks
    sceKernelNotifyCallback(cb, 0x31);
    SceUInt timeout = 1000;
    sceKernelWaitSemaCB(sema, 1, &timeout);
    print_recorded("semaphore wait");

    // Notified by another thread
    const SceUID thread = test_start_thread(notify_thread);
    notifier = thread;
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    sceKernelCheckCallback();
    print_recorded("notified by another thread");
    sceKernelDeleteThread(thread);

    // What another thread reports while a callback it has is pending in a wait without callbacks
    plain_sema = sceKernelCreateSema("plain", 0, 0, 1, NULL);
    const SceUID plain = test_start_thread(plain_thread);
    test_wait_until_waiting(plain);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(plain, &info);
    PRINT("plain wait: the callback notify field with none pending is 0", info.fNotifyCallback == 0);
    sceKernelNotifyCallback(plain_cb, 1);
    sceKernelGetThreadInfo(plain, &info);
    PRINT("plain wait: the callback notify field with one pending is 0", info.fNotifyCallback == 0);
    PRINT("plain wait: the callback notify field with one pending is 1", info.fNotifyCallback == 1);
    PRINT("plain wait: the callback notify field with one pending is the callback", info.fNotifyCallback == plain_cb);
    sceKernelSignalSema(plain_sema, 1);
    test_join_thread(plain);

    // A callback running on another core, seen from here
    sceKernelChangeThreadCpuAffinityMask(0, CPU_MASK_USER_0);
    busy_sema = sceKernelCreateSema("busy", 0, 0, 1, NULL);
    busy_result = 0xDEAD;
    const SceUID busy = sceKernelCreateThread("busy", busy_thread, 0x10000100, 0x4000, 0, CPU_MASK_USER_1, NULL);
    sceKernelStartThread(busy, 0, NULL);
    test_wait_until_waiting(busy);
    sceKernelNotifyCallback(busy_cb, 1);
    TEST_WAIT_FOR(in_callback);
    PRINT("running callback: it started", in_callback);
    sceKernelGetThreadInfo(busy, &info);
    PRINT("running callback: the thread's status", info.status);
    PRINT("running callback: its wait type", info.waitType);
    PRINT("running callback: its wait UID is the semaphore", info.waitId == busy_sema);
    SceKernelSemaInfo sema_info = { .size = sizeof(sema_info) };
    sceKernelGetSemaInfo(busy_sema, &sema_info);
    PRINT("running callback: the semaphore's waiters", sema_info.numWaitThreads);
    PRINT("running callback: signal the semaphore", sceKernelSignalSema(busy_sema, 1));
    sceKernelGetSemaInfo(busy_sema, &sema_info);
    PRINT("running callback: the count after the signal", sema_info.currentCount);
    PRINT("running callback: the waiters after the signal", sema_info.numWaitThreads);
    release = 1;
    test_join_thread(busy);
    PRINT("running callback: the wait", busy_result);
    sceKernelGetSemaInfo(busy_sema, &sema_info);
    PRINT("running callback: the count at the end", sema_info.currentCount);

    test_exit();
    return 0;
}
