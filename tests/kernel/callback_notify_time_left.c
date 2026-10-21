// When the time left of a wait with callbacks and a timeout is fixed, if a callback is notified to
// the waiting thread: at the notify, or later when the thread gets the CPU to run the callback.
// Thread T waits with callbacks for 3 s. The notify ends T's wait for the callback, but T can't run
// it until the main thread blocks, so the two moments are 500 ms apart. Before T runs, the main
// thread releases what T waits for. After its callback, T takes it at once and its wait returns 0
// without blocking again, so the time left it writes back to its timeout is the one fixed earlier.
// Every thread is pinned to core 0, and T runs at 0xB0, below the main thread at 0x60. The main
// thread keeps the core busy for 500 ms by calling sceKernelGetProcessTimeWide in a loop, which
// doesn't block, so T stays ready the whole time. The cases:
// - semaphore, notify first: the main thread notifies, keeps the core busy for 500 ms, signals the
//   semaphore and then blocks until T ends
// - semaphore, busy first: the control, which keeps the core busy for 500 ms before it notifies,
//   then signals right after the notify and blocks. The notify and T's run are close together, so
//   both moments give the same time left.
// - event flag, notify first: the same as the first case with an event flag that T waits on for bit
//   0x1 and the main thread sets
// T records the time right before its wait. The main thread records the time of the notify and
// computes what T's time left would be at that time, the 3 s minus the time from T's start to the
// notify. Each case prints whether T waits when the main thread notifies, T's wait result, how
// often T's callback ran, whether T's time left is within 200 ms of the one at the notify, and
// whether it is at least 300 ms less than the one at the notify. If the time left is fixed at the
// notify, the first of the two comparisons holds and the second doesn't. If it is fixed when T
// runs, it is the other way round in the first and third case.
//
// On a PS Vita TV on firmware 3.65 the time left is the one at the notify in every case, within 200
// ms, though T runs 500 ms after the notify in the first and the third case. T's callback runs once
// in each case.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define T_PRIORITY 0xB0
#define NOT_DONE 0xDEAD
#define WAIT_US 3000000
#define BUSY_US 500000
#define NEAR_US 200000
#define LESS_US 300000

typedef enum Kind { SEMA, FLAG } Kind;

static Kind kind;
static SceUID object;
static volatile SceUID callback;
static volatile int callback_runs, wait_result;
static volatile SceUInt time_left;
static volatile SceUInt64 t_start;

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    return 0;
}

// Creates a callback, records the time and waits with callbacks for 3 s
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
    SceUInt timeout = WAIT_US;
    SceUInt32 bits;
    t_start = sceKernelGetProcessTimeWide();
    if (kind == SEMA)
        wait_result = sceKernelWaitSemaCB(object, 1, &timeout);
    else
        wait_result = sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
    time_left = timeout;
    sceKernelDeleteCallback(callback);
    return 0;
}

// Keeps the core busy without blocking for the given time
static void keep_busy(SceUInt64 us) {
    const SceUInt64 end = sceKernelGetProcessTimeWide() + us;
    while (sceKernelGetProcessTimeWide() < end)
        ;
}

// Gives T what it waits for
static void release(void) {
    if (kind == SEMA)
        sceKernelSignalSema(object, 1);
    else
        sceKernelSetEventFlag(object, 0x1);
}

static void run_case(const char *name, Kind k, int busy_first) {
    kind = k;
    callback = 0;
    callback_runs = 0;
    wait_result = NOT_DONE;
    time_left = NOT_DONE;
    t_start = 0;
    if (k == SEMA)
        object = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    else
        object = sceKernelCreateEventFlag("flag", 0, 0, NULL);
    const SceUID t = sceKernelCreateThread("t", t_entry, T_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);

    const SceUInt32 wait_type = (k == SEMA ? WAIT_TYPE_SEMA : WAIT_TYPE_EVENT_FLAG) | WAIT_TYPE_CB;
    TEST_WAIT_FOR(test_in_wait(t, wait_type, object));
    const int waiting = test_in_wait(t, wait_type, object);
    if (busy_first)
        keep_busy(BUSY_US);
    const SceUInt64 notify_time = sceKernelGetProcessTimeWide();
    sceKernelNotifyCallback(callback, 1);
    if (!busy_first)
        keep_busy(BUSY_US);
    release();
    test_join_thread(t);

    const SceInt64 at_notify = (SceInt64)WAIT_US - (SceInt64)(notify_time - t_start);
    const SceInt64 less = at_notify - (SceInt64)time_left;
    PRINTF(waiting, "%s: T waits when the main thread notifies", name);
    PRINTF(wait_result, "%s: T's wait result", name);
    PRINTF(callback_runs, "%s: runs of T's callback", name);
    PRINTF(less >= -NEAR_US && less <= NEAR_US, "%s: the time left is within 200 ms of the one at the notify", name);
    PRINTF(less >= LESS_US, "%s: the time left is at least 300 ms less than the one at the notify", name);

    if (k == SEMA)
        sceKernelDeleteSema(object);
    else
        sceKernelDeleteEventFlag(object);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    run_case("semaphore, notify first", SEMA, 0);
    run_case("semaphore, busy first", SEMA, 1);
    run_case("event flag, notify first", FLAG, 0);

    test_exit();
    return 0;
}
