// Which outcome a wait gets when two things happen to it before its thread can run again. Waiter W
// runs on core 0 below a gate thread that spins there without syscalls, so once W's wait ends it
// stays ready until the gate lets go. The main thread runs on core 1. Cases, on a semaphore unless
// named: the timeout passes then a signal comes, a signal comes then the semaphore is deleted, a
// signal then a cancel, a signal then the timeout passes, a signal then a callback notify during a
// wait with callbacks, a callback notify then a signal during a wait with callbacks, and a mutex
// unlock to W then a delete of the mutex. Each prints W's status in between, W's result, the count
// left, and whether W's callback ran.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define W_PRIORITY 0xA0
#define SPINS 400000000

static SceUID sema, mutex, callback;
static volatile int gate_up, gate_open, result, done, callback_ran, callback_ready, use_cb, use_mutex;
static volatile SceUInt w_timeout;

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    gate_up = 1;
    for (volatile int n = 0; n < SPINS && !gate_open; n++)
        ;
    return 0;
}

static SceUID gate;

static void gate_close(void) {
    gate_up = gate_open = 0;
    gate = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);
}

static void gate_lift(void) {
    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
}

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_ran = 1;
    return 0;
}

static int w_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = w_timeout;
    if (use_mutex) {
        result = sceKernelLockMutex(mutex, 1, &timeout);
        if (result == 0)
            sceKernelUnlockMutex(mutex, 1);
    } else if (use_cb) {
        callback = sceKernelCreateCallback("cb", 0, callback_entry, NULL);
        callback_ready = 1;
        result = sceKernelWaitSemaCB(sema, 1, &timeout);
        // Runs a callback notified after the wait ended
        sceKernelCheckCallback();
        sceKernelDeleteCallback(callback);
    } else {
        result = sceKernelWaitSema(sema, 1, &timeout);
    }
    done = 1;
    return 0;
}

static SceUID w;

static void start_w(SceUInt timeout, int cb, int on_mutex) {
    result = 0xDEAD;
    done = callback_ran = callback_ready = 0;
    use_cb = cb;
    use_mutex = on_mutex;
    w_timeout = timeout;
    w = sceKernelCreateThread("w", w_entry, W_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(w, 0, NULL);
    test_wait_until_waiting(w);
    if (cb)
        TEST_WAIT_FOR(callback_ready);
}

static int status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

static int sema_count(void) {
    SceKernelSemaInfo info = { .size = sizeof(info) };
    if (sceKernelGetSemaInfo(sema, &info) < 0)
        return -1;
    return info.currentCount;
}

static void end_case(const char *name) {
    gate_lift();
    TEST_WAIT_FOR(done);
    test_join_thread(w);
    PRINTF(result, "%s: W's result", name);
    PRINTF(sema_count(), "%s: the count after W ran", name);
    PRINTF(callback_ran, "%s: W's callback ran", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);

    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    start_w(100000, 0, 0);
    gate_close();
    sceKernelDelayThread(300000);
    PRINT("timeout then signal: W's status once the timeout passed", status_of(w));
    PRINT("timeout then signal: the signal", sceKernelSignalSema(sema, 1));
    PRINT("timeout then signal: the count after the signal", sema_count());
    end_case("timeout then signal");
    sceKernelDeleteSema(sema);

    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    start_w(2 * TEST_WAIT_US, 0, 0);
    gate_close();
    sceKernelSignalSema(sema, 1);
    PRINT("signal then delete: W's status after the signal", status_of(w));
    PRINT("signal then delete: the delete", sceKernelDeleteSema(sema));
    end_case("signal then delete");

    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    start_w(2 * TEST_WAIT_US, 0, 0);
    gate_close();
    sceKernelSignalSema(sema, 1);
    int waiting = 0xDEAD;
    PRINT("signal then cancel: the cancel", sceKernelCancelSema(sema, 0, &waiting));
    PRINT("signal then cancel: the waiters it counts", waiting);
    end_case("signal then cancel");
    sceKernelDeleteSema(sema);

    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    start_w(100000, 0, 0);
    gate_close();
    sceKernelSignalSema(sema, 1);
    sceKernelDelayThread(300000);
    PRINT("signal then the timeout passes: W's status", status_of(w));
    end_case("signal then the timeout passes");
    sceKernelDeleteSema(sema);

    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    start_w(2 * TEST_WAIT_US, 1, 0);
    gate_close();
    sceKernelSignalSema(sema, 1);
    PRINT("with callbacks, signal then notify: the notify", sceKernelNotifyCallback(callback, 1));
    PRINT("with callbacks, signal then notify: W's status", status_of(w));
    end_case("with callbacks, signal then notify");
    sceKernelDeleteSema(sema);

    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    start_w(2 * TEST_WAIT_US, 1, 0);
    gate_close();
    PRINT("with callbacks, notify then signal: the notify", sceKernelNotifyCallback(callback, 1));
    PRINT("with callbacks, notify then signal: W's status after the notify", status_of(w));
    PRINT("with callbacks, notify then signal: the signal", sceKernelSignalSema(sema, 1));
    PRINT("with callbacks, notify then signal: the count after the signal", sema_count());
    end_case("with callbacks, notify then signal");
    sceKernelDeleteSema(sema);

    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    mutex = sceKernelCreateMutex("mutex", 0, 1, NULL);
    start_w(2 * TEST_WAIT_US, 0, 1);
    gate_close();
    PRINT("mutex unlock then delete: the unlock", sceKernelUnlockMutex(mutex, 1));
    PRINT("mutex unlock then delete: W's status after the unlock", status_of(w));
    PRINT("mutex unlock then delete: the delete", sceKernelDeleteMutex(mutex));
    end_case("mutex unlock then delete");
    sceKernelDeleteSema(sema);

    test_exit();
    return 0;
}
