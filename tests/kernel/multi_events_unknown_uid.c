// sceKernelWaitMultipleEvents with list entries that aren't usable events, next to real events that
// aren't set, so the call has to wait for them or not. Each case starts a thread that makes the
// call with a long timeout and prints whether the thread is waiting. If it waits, the main thread
// sets or cancels an event. Then the case prints the call's result and each result entry as the
// position of its event in the list (9 for UID 0x80028201, 0 for none of them), its result, its bits
// and its user data. The waiter runs on core 0 below the main thread, which also stays on core 0,
// so a thread that a set or cancel woke is still in no wait when the main thread looks at it, and
// "still waits" reads the same on every run. Cases:
// - AND on an unknown UID and an unset event A, then OR on the same list
// - AND on a deleted event's UID and A, on a semaphore's UID and A, on UID 0 and A
// - AND and OR on A and an unknown UID, the unusable entry listed last
// - AND on two unknown UIDs
// - AND on A twice, set once, with A manual reset and auto reset, and OR on A twice
// - AND on A and B while B is cancelled and A set later
// - AND on A and B while A is set and B is cancelled later
// - OR on A and B while B is cancelled
// The cancel cases come last, since they cancel an event a thread waits on from a list.
//
// On a PS Vita TV on firmware 3.65 an entry that isn't a usable event, an unknown UID, a deleted
// event, a semaphore or UID 0, finishes at once with 0x80028201, bits 0x10 and user data 0. An AND
// wait still waits for the other events, and an OR wait returns that entry alone. The entries come
// in the order they finished. An AND wait on A twice takes one set of a manual reset event for
// both, while an auto reset event needs a second set. A cancel of B finishes B's entry with
// 0x80028007 and reports 1 waiter, which ends an OR wait and an AND wait whose other events
// finished.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xB0
#define NOT_DONE 0xDEAD
#define UNKNOWN_UID 0x12345
#define SECOND_UNKNOWN_UID 0x23456
// What the call records for an entry whose event it can't find
#define UNKNOWN_EVENT_RESULT_UID 0x80028201

static WaitEvent list[2];
static ResultEvent results[2];
static SceUInt32 wait_mode;
static SceUID waiter_thread;
static volatile int wait_result;

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = 2 * TEST_WAIT_US;
    wait_result = sceKernelWaitMultipleEvents(list, 2, wait_mode, results, &timeout);
    return 0;
}

// Returns whether the waiter is in a wait on several events
static int in_wait(void) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(waiter_thread, &info) == 0 && info.waitType == WAIT_TYPE_MP_EVENTS;
}

// Starts the waiter on the two entries, which wait for EVENT_BIT_A, and returns whether the call waits
static int start_wait(const char *name, SceUInt32 mode, SceUID first, SceUID second) {
    list[0] = (WaitEvent){ first, EVENT_BIT_A };
    list[1] = (WaitEvent){ second, EVENT_BIT_A };
    wait_mode = mode;
    wait_result = NOT_DONE;
    sceClibMemset(results, 0xAA, sizeof(results));
    waiter_thread = sceKernelCreateThread("waiter", waiter, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(waiter_thread, 0, NULL);
    // The call either ends at once or waits
    TEST_WAIT_FOR(wait_result != NOT_DONE || in_wait());
    const int waits = in_wait();
    PRINTF(waits, "%s: the call waits", name);
    return waits;
}

// Returns the position of an entry's event in the list, counted from 1
static int item_of(SceUID id) {
    for (int i = 0; i < 2; i++) {
        if (list[i].eventId == id)
            return i + 1;
    }
    return id == (SceUID)UNKNOWN_EVENT_RESULT_UID ? 9 : 0;
}

// Joins the waiter and prints the call's result and the entries it returned
static void finish(const char *name) {
    test_join_thread(waiter_thread);
    PRINTF(wait_result, "%s: the call", name);
    for (int i = 0; i < 2 && i < wait_result; i++) {
        PRINTF(item_of(results[i].eventId), "%s: entry %d is the list's item", name, i);
        PRINTF(results[i].result, "%s: entry %d result", name, i);
        PRINTF(results[i].resultPattern, "%s: entry %d bits", name, i);
        PRINTF((int)results[i].userData, "%s: entry %d user data", name, i);
    }
}

static SceUInt32 bits_of(SceUID event) {
    SceUInt32 pattern = 0xDEAD;
    sceKernelGetEventPattern(event, &pattern);
    return pattern;
}

typedef enum Unusable { UNKNOWN, DELETED, SEMAPHORE, ZERO } Unusable;

// An unusable entry and the unset event A, listed in either order. If the call waits, A is set.
static void unusable_case(const char *name, SceUInt32 mode, Unusable kind, int unusable_first) {
    const SceUID a = sceKernelCreateSimpleEvent("a", 0, 0, NULL);
    SceUID unusable = UNKNOWN_UID;
    SceUID sema = 0;
    if (kind == DELETED) {
        unusable = sceKernelCreateSimpleEvent("deleted", 0, 0, NULL);
        sceKernelDeleteSimpleEvent(unusable);
    } else if (kind == SEMAPHORE) {
        sema = unusable = sceKernelCreateSema("sema", 0, 1, 1, NULL);
    } else if (kind == ZERO) {
        unusable = 0;
    }
    const int waits = unusable_first ? start_wait(name, mode, unusable, a) : start_wait(name, mode, a, unusable);
    if (waits) {
        sceKernelSetEvent(a, EVENT_BIT_A, 0x77);
        PRINTF(in_wait(), "%s: the call still waits after the set of A", name);
    }
    finish(name);
    sceKernelDeleteSimpleEvent(a);
    if (sema)
        sceKernelDeleteSema(sema);
}

// The same event twice. If the call waits, A is set once, and again if the call still waits.
static void same_event_case(const char *name, SceUInt32 mode, SceUInt32 attr) {
    const SceUID a = sceKernelCreateSimpleEvent("a", attr, 0, NULL);
    if (start_wait(name, mode, a, a)) {
        sceKernelSetEvent(a, EVENT_BIT_A, 0x77);
        const int still = in_wait();
        PRINTF(still, "%s: the call still waits after one set", name);
        if (still)
            sceKernelSetEvent(a, EVENT_BIT_A, 0x88);
    }
    finish(name);
    PRINTF(bits_of(a), "%s: the bits of A after", name);
    sceKernelDeleteSimpleEvent(a);
}

// Events A and B, with B cancelled while the call waits. In an AND wait, A is set before or after
// the cancel. In an OR wait, A is only set if the call still waits after the cancel.
static void cancel_case(const char *name, SceUInt32 mode, int set_first) {
    const SceUID a = sceKernelCreateSimpleEvent("a", 0, 0, NULL);
    const SceUID b = sceKernelCreateSimpleEvent("b", 0, 0, NULL);
    if (start_wait(name, mode, a, b)) {
        if (set_first) {
            sceKernelSetEvent(a, EVENT_BIT_A, 0x77);
            PRINTF(in_wait(), "%s: the call still waits after the set of A", name);
        }
        SceUInt32 waiters = 0xDEAD;
        PRINTF(sceKernelCancelEvent(b, &waiters), "%s: the cancel of B", name);
        PRINTF(waiters, "%s: the waiters the cancel reports", name);
        const int still = in_wait();
        PRINTF(still, "%s: the call still waits after the cancel", name);
        if (still && !set_first)
            sceKernelSetEvent(a, EVENT_BIT_A, 0x77);
    }
    finish(name);
    sceKernelDeleteSimpleEvent(a);
    sceKernelDeleteSimpleEvent(b);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    unusable_case("AND, an unknown UID and A", EVENT_WAIT_MODE_AND, UNKNOWN, 1);
    unusable_case("OR, an unknown UID and A", EVENT_WAIT_MODE_OR, UNKNOWN, 1);
    unusable_case("AND, a deleted event and A", EVENT_WAIT_MODE_AND, DELETED, 1);
    unusable_case("AND, a semaphore and A", EVENT_WAIT_MODE_AND, SEMAPHORE, 1);
    unusable_case("AND, UID 0 and A", EVENT_WAIT_MODE_AND, ZERO, 1);
    unusable_case("AND, A and an unknown UID", EVENT_WAIT_MODE_AND, UNKNOWN, 0);
    unusable_case("OR, A and an unknown UID", EVENT_WAIT_MODE_OR, UNKNOWN, 0);

    // Nothing to wait for, so the wait only ends by its timeout if the call does wait
    start_wait("AND, two unknown UIDs", EVENT_WAIT_MODE_AND, UNKNOWN_UID, SECOND_UNKNOWN_UID);
    finish("AND, two unknown UIDs");

    same_event_case("AND, A twice", EVENT_WAIT_MODE_AND, EVENT_ATTR_MANUAL_RESET);
    same_event_case("AND, A twice, auto reset", EVENT_WAIT_MODE_AND, EVENT_ATTR_AUTO_RESET);
    same_event_case("OR, A twice", EVENT_WAIT_MODE_OR, EVENT_ATTR_MANUAL_RESET);

    cancel_case("AND, B cancelled then A set", EVENT_WAIT_MODE_AND, 0);
    cancel_case("AND, A set then B cancelled", EVENT_WAIT_MODE_AND, 1);
    cancel_case("OR, B cancelled", EVENT_WAIT_MODE_OR, 0);

    test_exit();
    return 0;
}
