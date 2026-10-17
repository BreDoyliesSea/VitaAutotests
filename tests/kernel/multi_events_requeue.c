// Whether a priority change of a thread in sceKernelWaitMultipleEvents moves it in the queue of
// each event it waits on. Single waiters SA and SB wait at 0x88 for bit EVENT_BIT_A of the auto
// reset simple events a and b, then M waits at 0x90 for that bit of both with EVENT_WAIT_MODE_OR.
// An auto reset event set goes to the first waiter in that event's queue only (handoff_more). Then
// M is raised to 0x80, or not, and one event is set twice, the second time once the first set woke
// someone. Cases, with events that have SCE_KERNEL_ATTR_THREAD_PRIO unless the case says otherwise:
// - no change, a is set twice
// - M raised, a is set twice
// - M raised, b is set twice
// - FIFO events, M raised, a is set twice
// Each case prints which waiter each set woke, as 0xA for SA, 0xB for SB and 0xC for M, and M's
// result. The waits left end when the events are deleted.
//
// On a PS Vita TV on firmware 3.65 the first set goes to the single waiter without the change and
// with FIFO events, and to M once M is raised, for a and for b, so the raise moves M in the queue
// of each event it waits on. The second set goes to the single waiter M left, and M's wait returns
// 1, with the event whose set reached it.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID events[2];
static SceUID threads[3];
static volatile int result[3], woken_count, woke[3];
static ResultEvent results[2];

static void record(int who) {
    const int n = __atomic_fetch_add(&woken_count, 1, __ATOMIC_SEQ_CST);
    if (n < 3)
        woke[n] = who;
}

// Waiters 0 and 1 wait on events[0] and events[1] alone, waiter 2 on both
static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 timeout = 2 * TEST_WAIT_US;
    if (i < 2) {
        SceUInt32 pattern;
        SceUInt64 user_data;
        result[i] = sceKernelWaitEvent(events[i], EVENT_BIT_A, &pattern, &user_data, &timeout);
    } else {
        WaitEvent wait[2] = { { events[0], EVENT_BIT_A }, { events[1], EVENT_BIT_A } };
        result[i] = sceKernelWaitMultipleEvents(wait, 2, EVENT_WAIT_MODE_OR, results, &timeout);
    }
    if (result[i] >= 0)
        record(0xA + i);
    return 0;
}

static void run(const char *name, SceUInt32 attr, int raise, int set) {
    for (int e = 0; e < 2; e++)
        events[e] = sceKernelCreateSimpleEvent("event", attr | EVENT_ATTR_AUTO_RESET, 0, NULL);
    woken_count = 0;
    for (int i = 0; i < 3; i++) {
        result[i] = 0xDEAD;
        woke[i] = 0;
        threads[i] = sceKernelCreateThread("waiter", waiter, i < 2 ? 0x88 : 0x90, 0x4000, 0, 0,
                                           NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }

    int changed = 0;
    if (raise)
        changed = sceKernelChangeThreadPriority(threads[2], 0x80);
    sceKernelSetEvent(events[set], EVENT_BIT_A, 0);
    TEST_WAIT_FOR(woken_count >= 1);
    sceKernelSetEvent(events[set], EVENT_BIT_A, 0);
    TEST_WAIT_FOR(woken_count >= 2);

    for (int e = 0; e < 2; e++)
        sceKernelDeleteSimpleEvent(events[e]);
    for (int i = 0; i < 3; i++)
        test_join_thread(threads[i]);
    // Read once every waiter ended, since a waiter counts itself before it writes its record
    const int first = woke[0], second = woke[1];
    if (raise)
        PRINTF(changed, "%s: raise M", name);
    PRINTF(first, "%s: the waiter the first set woke", name);
    PRINTF(second, "%s: the waiter the second set woke", name);
    PRINTF(result[2], "%s: M's result", name);
    if (result[2] > 0) {
        int which = 0;
        if (results[0].eventId == events[0])
            which = 0xA;
        else if (results[0].eventId == events[1])
            which = 0xB;
        PRINTF(which, "%s: the event M's first result is for, 0xA for a and 0xB for b", name);
    }
}

int main(void) {
    const SceUInt32 prio = SCE_KERNEL_ATTR_THREAD_PRIO;
    run("no change, a set twice", prio, 0, 0);
    run("M raised, a set twice", prio, 1, 0);
    run("M raised, b set twice", prio, 1, 1);
    run("FIFO events, M raised, a set twice", 0, 1, 0);

    test_exit();
    return 0;
}
