// Whether a wait with callbacks rejects a bad argument, or another thread's hold, before it runs
// the callbacks that are pending, and whether it writes its out bits before it runs them. Every
// case runs on its own thread T, pinned to core 0, as the main thread does too. T creates a
// callback, notifies it once so that it is pending, makes the wait with callbacks and records the
// wait's result and how often the callback had run when the wait returned. Then T calls
// sceKernelCheckCallback and records its result and how often the callback had run after it. The
// main thread waits for T to end and prints. Every wait has a finite timeout. The cases:
// - sceKernelWaitEventFlagCB with the mode 0x8
// - the same with both clear modes together
// - the same with the pattern 0
// - the same on an event flag without SCE_EVENT_WAITMULTIPLE that another thread H already waits
//   on, for a bit that is not set, while T's bit is set. The main thread sets H's bit afterwards.
// - the same with the bits already set, so that the wait ends at once. The out bits start as 0xDEAD
//   and the callback records what it reads in them
// - sceKernelWaitSemaCB with a count of 0, and with a count above the semaphore's maximum
// - sceKernelWaitEventFlagCB with an unknown UID
// Each case prints the wait's result, the runs of the callback when the wait returned, the check's
// result and the runs after the check. The case with the bits already set also prints the out bits
// that the callback read and the out bits after the wait.
//
// On a PS Vita TV on firmware 3.65 each bad argument, the other thread's hold and the unknown UID
// end the wait before the callback runs, so the check that follows runs it and returns 1. A wait
// that takes the bits at once runs the callback before it writes the out bits, so the callback
// reads 0xDEAD.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define NOT_RUN 0xBEEF
#define UNKNOWN_UID 0x12345

typedef enum Kind { BAD_MODE, BOTH_CLEAR_MODES, PATTERN_ZERO, HELD, TAKE_AT_ONCE, SEMA_ZERO, SEMA_ABOVE_MAX, UNKNOWN_FLAG } Kind;

static Kind kind;
static SceUID object, callback;
static unsigned int out_bits;
static volatile int callback_runs, done;
static volatile unsigned int callback_saw;
static volatile int wait_result, runs_in_wait, check_result, runs_after_check;

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_saw = *(volatile unsigned int *)&out_bits;
    callback_runs++;
    return 0;
}

static int make_wait(void) {
    SceUInt timeout = TEST_WAIT_US;
    switch (kind) {
    case BAD_MODE: return sceKernelWaitEventFlagCB(object, 0x1, 0x8, &out_bits, &timeout);
    case BOTH_CLEAR_MODES: return sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR | SCE_EVENT_WAITCLEAR_PAT, &out_bits, &timeout);
    case PATTERN_ZERO: return sceKernelWaitEventFlagCB(object, 0x0, SCE_EVENT_WAITOR, &out_bits, &timeout);
    case HELD: return sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR, &out_bits, &timeout);
    case TAKE_AT_ONCE: return sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &out_bits, &timeout);
    case SEMA_ZERO: return sceKernelWaitSemaCB(object, 0, &timeout);
    case SEMA_ABOVE_MAX: return sceKernelWaitSemaCB(object, 3, &timeout);
    case UNKNOWN_FLAG: return sceKernelWaitEventFlagCB(UNKNOWN_UID, 0x1, SCE_EVENT_WAITOR, &out_bits, &timeout);
    }
    return 0;
}

// Notifies its own callback once, then makes the wait of the case and checks the callbacks
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("pending", 0, callback_entry, NULL);
    sceKernelNotifyCallback(callback, 1);
    wait_result = make_wait();
    runs_in_wait = callback_runs;
    check_result = sceKernelCheckCallback();
    runs_after_check = callback_runs;
    sceKernelDeleteCallback(callback);
    done = 1;
    return 0;
}

// Waits on the flag for bit 0x2, which is not set, until the main thread sets it
static int h_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    unsigned int bits;
    SceUInt timeout = 2 * TEST_WAIT_US;
    sceKernelWaitEventFlag(object, 0x2, SCE_EVENT_WAITOR, &bits, &timeout);
    return 0;
}

static SceUID start(SceKernelThreadEntry entry) {
    const SceUID thread = sceKernelCreateThread("t", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    return thread;
}

static void run_case(const char *name, Kind k) {
    kind = k;
    out_bits = NOT_DONE;
    callback_saw = NOT_RUN;
    wait_result = runs_in_wait = check_result = runs_after_check = NOT_DONE;
    callback_runs = done = 0;
    SceUID holder = -1;
    switch (k) {
    case HELD:
    case BAD_MODE:
    case BOTH_CLEAR_MODES:
    case PATTERN_ZERO:
        object = sceKernelCreateEventFlag("flag", 0, 0x1, NULL);
        break;
    case TAKE_AT_ONCE: object = sceKernelCreateEventFlag("flag", 0, 0x7, NULL); break;
    case SEMA_ZERO:
    case SEMA_ABOVE_MAX: object = sceKernelCreateSema("sema", 0, 1, 2, NULL); break;
    case UNKNOWN_FLAG: object = -1; break;
    }
    if (k == HELD) {
        holder = start(h_entry);
        TEST_WAIT_FOR(test_in_wait(holder, WAIT_TYPE_EVENT_FLAG, object));
    }
    const SceUID t = start(t_entry);
    TEST_WAIT_FOR(done);
    test_join_thread(t);
    if (k == HELD) {
        sceKernelSetEventFlag(object, 0x2);
        test_join_thread(holder);
    }
    if (k == SEMA_ZERO || k == SEMA_ABOVE_MAX)
        sceKernelDeleteSema(object);
    else if (k != UNKNOWN_FLAG)
        sceKernelDeleteEventFlag(object);

    PRINTF(wait_result, "%s: the wait", name);
    PRINTF(runs_in_wait, "%s: runs of the callback when the wait returned", name);
    PRINTF(check_result, "%s: the check's result", name);
    PRINTF(runs_after_check, "%s: runs of the callback after the check", name);
    if (k == TAKE_AT_ONCE) {
        PRINTF(callback_saw, "%s: the out bits the callback read", name);
        PRINTF(out_bits, "%s: the out bits after the wait", name);
    }
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    run_case("event flag, mode 0x8", BAD_MODE);
    run_case("event flag, both clear modes", BOTH_CLEAR_MODES);
    run_case("event flag, pattern 0", PATTERN_ZERO);
    run_case("event flag, held by another thread", HELD);
    run_case("event flag, bits already set", TAKE_AT_ONCE);
    run_case("semaphore, count 0", SEMA_ZERO);
    run_case("semaphore, count above the maximum", SEMA_ABOVE_MAX);
    run_case("event flag, unknown UID", UNKNOWN_FLAG);

    test_exit();
    return 0;
}
