// How one set of an event flag with several waiters is handed out: which waiters wake, which
// bits each one gets, and what the clears of earlier waiters leave for later ones, in a FIFO
// queue and in a priority queue where the last waiter to queue has the highest priority. Also a
// waiter whose bits came in two sets with a clear by another waiter in between, and a clear
// while waiters wait. A waiter that doesn't wake times out after TEST_WAIT_US, far past the
// checks that expect it to wait.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAX_WAITERS 3

typedef struct Waiter {
    SceUInt32 pattern, mode;
    int priority;
} Waiter;

static SceUID flag;
static Waiter waiters[MAX_WAITERS];
static SceUID threads[MAX_WAITERS];
static volatile int result[MAX_WAITERS], done[MAX_WAITERS];
static SceUInt32 bits[MAX_WAITERS];

static int waiter_entry(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = TEST_WAIT_US;
    result[i] = sceKernelWaitEventFlag(flag, waiters[i].pattern, waiters[i].mode, &bits[i], &timeout);
    done[i] = 1;
    return 0;
}

// Creates the flag and starts the waiters in order, each once the one before waits
static void start(SceUInt32 attr, SceUInt32 init, const Waiter *list, int count) {
    flag = sceKernelCreateEventFlag("flag", attr | SCE_EVENT_WAITMULTIPLE, init, NULL);
    for (int i = 0; i < count; i++) {
        waiters[i] = list[i];
        result[i] = 0xDEAD;
        done[i] = 0;
        bits[i] = 0xDEAD;
        threads[i] = sceKernelCreateThread("waiter", waiter_entry, list[i].priority, 0x4000, 0, 0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }
}

static SceUInt32 pattern(void) {
    SceKernelEventFlagInfo info = { .size = sizeof(info) };
    sceKernelGetEventFlagInfo(flag, &info);
    return info.currentPattern;
}

static int waiting(void) {
    SceKernelEventFlagInfo info = { .size = sizeof(info) };
    sceKernelGetEventFlagInfo(flag, &info);
    return info.numWaitThreads;
}

// Waits until every waiter woke or timed out, then prints each one's result and bits
static void finish(const char *name, int count) {
    for (int i = 0; i < count; i++)
        test_join_thread(threads[i]);
    for (int i = 0; i < count; i++) {
        PRINTF(result[i], "%s: waiter %d's result", name, i + 1);
        PRINTF(result[i] == 0 ? bits[i] : 0, "%s: the bits waiter %d got", name, i + 1);
    }
    PRINTF(pattern(), "%s: the bits left", name);
    sceKernelDeleteEventFlag(flag);
}

// Waits up to 100 ms for the waiters a set can wake to wake, and returns how many still wait
static int settle(int count) {
    int awake = 0;
    for (int poll = 0; poll < 100; poll++) {
        awake = 0;
        for (int i = 0; i < count; i++)
            awake += done[i];
        if (awake == count)
            break;
        sceKernelDelayThread(1000);
    }
    return count - awake;
}

#define DEFAULT 0xA0
#define HIGH 0x80

int main(void) {
    // Waiter 1 takes 0x1 and clears it, waiter 2 takes 0x1 or 0x2 and keeps them, waiter 3 needs
    // 0x2 and clears everything
    const Waiter chain[3] = {
        { 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, DEFAULT },
        { 0x3, SCE_EVENT_WAITOR, DEFAULT },
        { 0x2, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR, DEFAULT },
    };
    start(0, 0, chain, 3);
    PRINT("FIFO, a set of 0x3", sceKernelSetEventFlag(flag, 0x3));
    PRINT("FIFO, after a set of 0x3: waiters left", settle(3));
    finish("FIFO, a set of 0x3", 3);

    // The same, with waiter 3 at a higher priority in a priority queue
    Waiter prio_chain[3] = { chain[0], chain[1], chain[2] };
    prio_chain[2].priority = HIGH;
    start(SCE_KERNEL_ATTR_THREAD_PRIO, 0, prio_chain, 3);
    PRINT("priority, a set of 0x3", sceKernelSetEventFlag(flag, 0x3));
    PRINT("priority, after a set of 0x3: waiters left", settle(3));
    finish("priority, a set of 0x3", 3);

    // Waiter 1 needs 0x3 and clears what it waited for, waiter 2 takes 0x4
    const Waiter partial[2] = {
        { 0x3, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR_PAT, DEFAULT },
        { 0x4, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, DEFAULT },
    };
    start(0, 0, partial, 2);
    PRINT("a set of 0x7", sceKernelSetEventFlag(flag, 0x7));
    PRINT("after a set of 0x7: waiters left", settle(2));
    finish("a set of 0x7", 2);

    // Waiter 1 needs 0x3, waiter 2 takes 0x1 and clears everything. 0x1 then 0x2 are set.
    const Waiter split[2] = {
        { 0x3, SCE_EVENT_WAITAND, DEFAULT },
        { 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, DEFAULT },
    };
    start(0, 0, split, 2);
    sceKernelSetEventFlag(flag, 0x1);
    PRINT("a set of 0x1 for a later waiter: waiters left", settle(2));
    PRINT("a set of 0x1 for a later waiter: the bits after it", pattern());
    sceKernelSetEventFlag(flag, 0x2);
    PRINT("then a set of 0x2: waiters left", settle(2));
    finish("a set of 0x1 then 0x2", 2);

    // A clear while a waiter waits for bits that are partly there
    const Waiter one[1] = { { 0x3, SCE_EVENT_WAITAND, DEFAULT } };
    start(0, 0x1, one, 1);
    PRINT("a clear with a waiter", sceKernelClearEventFlag(flag, 0));
    PRINT("after the clear: waiters", waiting());
    sceKernelSetEventFlag(flag, 0x2);
    PRINT("a set of 0x2 after the clear: waiters left", settle(1));
    finish("a clear then a set of 0x2", 1);

    test_exit();
    return 0;
}
