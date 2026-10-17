// Whether a priority change of an event flag waiter changes what one set does, when a waiter that
// clears the bit comes before one that doesn't. Waiter A waits for bit 0x1 with SCE_EVENT_WAITCLEAR
// and queues first, then B waits for the same bit without clearing it. If a set wakes the waiters
// it satisfies in queue order, a set of 0x1 only wakes B when B comes first. Cases, with at most
// one priority change before the set:
// - priority queue, A above B, no change
// - priority queue: A drops below B, or B is raised above A
// - priority queue, A and B at the same priority: A is set to the priority it has
// - FIFO queue: A drops below B, or A is set to the priority it has
// Each case prints which waiters were woken within 500 ms of the change, then of the set, and the
// bits left. A waiter the set doesn't wake ends when the flag is deleted.
//
// On a PS Vita TV on firmware 3.65 no change wakes a waiter. In a priority queue the set wakes only
// A while A comes first, and both once B is ahead of A, whether A dropped, B was raised, or A was
// set to its own priority behind an equal B. So a set wakes the waiters it satisfies in queue
// order, and A's clear only hides the bit from the waiters behind it. The FIFO queue wakes only A.
// No bits are left after any set.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define HIGH 0x78
#define A_PRIORITY 0x80
#define B_PRIORITY 0x88
#define LOW 0x90

static const char *const names[2] = { "A", "B" };

static SceUID flag;
static SceUID threads[2];
static volatile int result[2], done[2];
static unsigned int bits[2];

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    const unsigned int mode = SCE_EVENT_WAITOR | (i == 0 ? SCE_EVENT_WAITCLEAR : 0);
    result[i] = sceKernelWaitEventFlag(flag, 0x1, mode, &bits[i], &timeout);
    done[i] = 1;
    return 0;
}

// Waits up to 500 ms for both waiters to be done, for an outcome that would come at once
static void settle(void) {
    for (int t = 0; t < 500 && !(done[0] && done[1]); t++)
        sceKernelDelayThread(1000);
}

static unsigned int pattern(void) {
    SceKernelEventFlagInfo info = { .size = sizeof(info) };
    sceKernelGetEventFlagInfo(flag, &info);
    return info.currentPattern;
}

// who < 0 makes no change
static void run(const char *name, SceUInt32 attr, int a_priority, int b_priority, int who,
                int priority) {
    flag = sceKernelCreateEventFlag("flag", attr | SCE_EVENT_WAITMULTIPLE, 0, NULL);
    const int priorities[2] = { a_priority, b_priority };
    for (int i = 0; i < 2; i++) {
        result[i] = 0xDEAD;
        done[i] = 0;
        bits[i] = 0;
        threads[i] = sceKernelCreateThread("waiter", waiter, priorities[i], 0x4000, 0, 0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }

    int changed = 0;
    if (who >= 0)
        changed = sceKernelChangeThreadPriority(threads[who], priority);
    settle();
    const int done_after_change[2] = { done[0], done[1] };
    sceKernelSetEventFlag(flag, 0x1);
    settle();
    const unsigned int left = pattern();

    if (who >= 0)
        PRINTF(changed, "%s: the change", name);
    for (int i = 0; i < 2; i++)
        PRINTF(done_after_change[i], "%s: %s is done after the change", name, names[i]);
    for (int i = 0; i < 2; i++)
        PRINTF(done[i], "%s: %s is done after the set", name, names[i]);
    PRINTF(left, "%s: the bits after the set", name);

    sceKernelDeleteEventFlag(flag);
    for (int i = 0; i < 2; i++)
        test_join_thread(threads[i]);
    for (int i = 0; i < 2; i++) {
        PRINTF(result[i], "%s: %s's result", name, names[i]);
        PRINTF(bits[i], "%s: %s's bits", name, names[i]);
    }
}

int main(void) {
    const SceUInt32 prio = SCE_KERNEL_ATTR_THREAD_PRIO;
    run("priority queue, no change", prio, A_PRIORITY, B_PRIORITY, -1, 0);
    run("priority queue, A drops below B", prio, A_PRIORITY, B_PRIORITY, 0, LOW);
    run("priority queue, B raised above A", prio, A_PRIORITY, B_PRIORITY, 1, HIGH);
    run("priority queue, A set to its own priority", prio, A_PRIORITY, A_PRIORITY, 0, A_PRIORITY);
    run("FIFO queue, A drops below B", 0, A_PRIORITY, B_PRIORITY, 0, LOW);
    run("FIFO queue, A set to its own priority", 0, A_PRIORITY, A_PRIORITY, 0, A_PRIORITY);

    test_exit();
    return 0;
}
