// The delay calls, plain, with callbacks and their 200 variants: what they return for a range of
// delays and that each lasts at least as long as asked, the status and wait fields another thread
// sees during each, and a callback run in the middle of a delay with callbacks.
// Times are printed as comparisons with wide margins.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// Generous, so a slow system doesn't fail the test
#define MAX_OVERSHOOT_US 200000

enum Kind { PLAIN, CB, PLAIN_200, CB_200, KIND_COUNT };
static const char *const kind_names[] = { "delay", "delay with callbacks", "delay 200", "delay 200 with callbacks" };

static int delay(int kind, SceUInt32 us) {
    switch (kind) {
    case PLAIN: return sceKernelDelayThread(us);
    case CB: return sceKernelDelayThreadCB(us);
    case PLAIN_200: return sceKernelDelayThread200(us);
    default: return sceKernelDelayThreadCB200(us);
    }
}

static volatile int delay_result, delay_done;
static volatile SceUInt64 delay_elapsed;
static volatile int helper_kind;
static volatile SceUInt32 helper_us;
static SceUID cb;
static volatile int calls;

// Delays in the way helper_kind and helper_us say, and records the result and the time it took
static int delay_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    delay_result = delay(helper_kind, helper_us);
    delay_elapsed = sceKernelGetProcessTimeWide() - start;
    delay_done = 1;
    return 0;
}

static SceUID start_delay(int kind, SceUInt32 us) {
    helper_kind = kind;
    helper_us = us;
    delay_result = 0xDEAD;
    delay_done = 0;
    return test_start_thread(delay_thread);
}

// The callback a delay with callbacks runs in the middle, which itself delays for 400 ms
static int slow_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    calls++;
    sceKernelDelayThread(400000);
    return 0;
}

static int callback_delay_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    cb = sceKernelCreateCallback("slow", 0, slow_callback, NULL);
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    delay_result = sceKernelDelayThreadCB(200000);
    delay_elapsed = sceKernelGetProcessTimeWide() - start;
    sceKernelDeleteCallback(cb);
    delay_done = 1;
    return 0;
}

static SceUInt32 wait_type(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 ? info.waitType : 0;
}

int main(void) {
    static const SceUInt32 delays[] = { 0, 1, 100, 199, 200, 201, 20000 };
    for (int kind = 0; kind < KIND_COUNT; kind++) {
        for (unsigned int i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
            const SceUInt64 start = sceKernelGetProcessTimeWide();
            const int result = delay(kind, delays[i]);
            const SceUInt64 elapsed = sceKernelGetProcessTimeWide() - start;
            PRINTF(result, "%s of %u", kind_names[kind], (unsigned)delays[i]);
            if (result != 0)
                continue;
            PRINTF(elapsed >= delays[i], "%s of %u: it lasts at least as asked", kind_names[kind], (unsigned)delays[i]);
            PRINTF(elapsed < delays[i] + MAX_OVERSHOOT_US, "%s of %u: and not much longer", kind_names[kind], (unsigned)delays[i]);
            if (kind >= PLAIN_200 && delays[i] < 200)
                PRINTF(elapsed >= 200, "%s of %u: it lasts at least 200 microseconds", kind_names[kind], (unsigned)delays[i]);
        }
    }

    // What another thread sees during each delay
    for (int kind = 0; kind < KIND_COUNT; kind++) {
        const SceUID thread = start_delay(kind, 300000);
        TEST_WAIT_FOR(wait_type(thread) != 0 || delay_done);
        SceKernelThreadInfo info = { .size = sizeof(info) };
        sceKernelGetThreadInfo(thread, &info);
        PRINTF(info.status, "%s, seen by another thread: the status", kind_names[kind]);
        PRINTF(info.waitType, "%s, seen by another thread: the wait type", kind_names[kind]);
        PRINTF(info.waitId == 0, "%s, seen by another thread: the wait UID is 0", kind_names[kind]);
        PRINTF(info.waitId == thread, "%s, seen by another thread: the wait UID is the thread", kind_names[kind]);
        if (info.waitId != 0 && info.waitId != thread)
            PRINTF(get_uid_class(info.waitId), "%s, seen by another thread: the class of the wait UID", kind_names[kind]);
        test_join_thread(thread);
        PRINTF(delay_result, "%s, seen by another thread: its result", kind_names[kind]);
    }

    // A callback that runs for 400 ms in the middle of a 200 ms delay with callbacks. The delay ends
    // after 400 ms if the callback's time counts toward the delay, or after 600 ms if it doesn't.
    calls = 0;
    delay_done = 0;
    SceUID thread = test_start_thread(callback_delay_thread);
    TEST_WAIT_FOR(wait_type(thread) == (WAIT_TYPE_DELAY | WAIT_TYPE_CB));
    sceKernelNotifyCallback(cb, 1);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitThreadEnd(thread, NULL, &timeout);
    sceKernelDeleteThread(thread);
    PRINT("a callback during a delay: runs of the callback", calls);
    PRINT("a callback during a delay: the delay's result", delay_result);
    PRINT("a callback during a delay: the delay lasts at least 400 ms", delay_elapsed >= 400000);
    PRINT("a callback during a delay: the delay lasts less than 500 ms", delay_elapsed < 500000);

    test_exit();
    return 0;
}
