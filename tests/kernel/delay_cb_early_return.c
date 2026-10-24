// What sceKernelDelayThreadCB returns when a callback is pending at its start and notifies itself on
// its first run, and how a wait on a semaphore with callbacks compares under the same callback. The
// main thread creates the callback and notifies it, which only leaves it pending since the thread
// isn't waiting. The callback counts its runs and notifies itself on its first run, so a callback
// is pending again after the first run. Then the main thread calls the wait with a timeout of 1000
// ms and, after it returned, a sceKernelCheckCallback. Every case has a fresh callback and prints
// the wait's result, whether it took at least 500 ms, how often the callback had run when the wait
// returned, and how often it had run after the check. The cases:
// - sceKernelDelayThreadCB for 1000000 microseconds
// - sceKernelWaitSemaCB on a semaphore at 0 that nothing signals, with a timeout of 1000 ms
//
// On a PS Vita TV on firmware 3.65 the delay returns 0 right after the callback's first run, so it
// takes under 500 ms and the callback ran once when it returned. The callback is still pending
// then, so the check runs it a second time. The semaphore wait runs the callback a second time
// before it sleeps, and returns 0x80028005 after the whole second, with two runs when it returned
// and none left for the check.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define LONG_US 1000000
#define THRESHOLD_US 500000

typedef enum Kind { DELAY, SEMA } Kind;

static SceUID callback;
static volatile int callback_runs;

// Counts its runs and notifies itself on the first one
static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    if (callback_runs == 1)
        sceKernelNotifyCallback(callback, 1);
    return 0;
}

static void run_case(const char *name, Kind kind) {
    callback_runs = 0;
    callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
    const SceUID sema = kind == SEMA ? sceKernelCreateSema("sema", 0, 0, 1, NULL) : 0;
    sceKernelNotifyCallback(callback, 1);

    SceUInt timeout = LONG_US;
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    const int result = kind == DELAY ? sceKernelDelayThreadCB(LONG_US) : sceKernelWaitSemaCB(sema, 1, &timeout);
    const SceUInt64 elapsed = sceKernelGetProcessTimeWide() - start;
    const int runs_at_return = callback_runs;
    sceKernelCheckCallback();
    const int runs_after_check = callback_runs;

    sceKernelDeleteCallback(callback);
    if (sema)
        sceKernelDeleteSema(sema);

    PRINTF(result, "%s: the result", name);
    PRINTF(elapsed >= THRESHOLD_US, "%s: it took at least 500 ms", name);
    PRINTF(runs_at_return, "%s: runs of the callback when it returned", name);
    PRINTF(runs_after_check, "%s: runs of the callback after a check", name);
}

int main(void) {
    run_case("delay", DELAY);
    run_case("semaphore wait", SEMA);

    test_exit();
    return 0;
}
