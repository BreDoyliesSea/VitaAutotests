// Whether a semaphore wait or poll that the count covers takes it while a waiter that needs more
// is queued before it. Waiter A needs 3 and waits on a semaphore that holds 2. Then thread B, at a
// priority above, at or below A's, waits for 1 or polls 1, in a FIFO and in a priority queue. Each
// case prints whether B waits, the count and the waiters, then cancels the semaphore, which ends
// whatever still waits, and prints how A and B ended.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define A_PRIORITY 0x80
#define NOT_DONE 0xDEAD

enum Kind { WAIT, POLL };

static SceUID sema;
static volatile int a_result, b_result, b_kind;

static int waiter_a(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    a_result = sceKernelWaitSema(sema, 3, &timeout);
    return 0;
}

static int thread_b(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    b_result = b_kind == WAIT ? sceKernelWaitSema(sema, 1, &timeout) : sceKernelPollSema(sema, 1);
    return 0;
}

static void run_case(SceUInt32 attr, const char *queue, int kind, int b_priority, const char *where) {
    sema = sceKernelCreateSema("sema", attr, 2, 10, NULL);
    a_result = b_result = NOT_DONE;
    b_kind = kind;
    const SceUID a = sceKernelCreateThread("A", waiter_a, A_PRIORITY, 0x4000, 0, 0, NULL);
    sceKernelStartThread(a, 0, NULL);
    test_wait_until_waiting(a);
    const SceUID b = sceKernelCreateThread("B", thread_b, b_priority, 0x4000, 0, 0, NULL);
    sceKernelStartThread(b, 0, NULL);
    // B either ends at once or waits behind A
    TEST_WAIT_FOR(b_result != NOT_DONE || test_in_wait(b, WAIT_TYPE_SEMA, sema));
    const int b_waits = test_in_wait(b, WAIT_TYPE_SEMA, sema);
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(sema, &info);

    const char *what = kind == WAIT ? "B waits for 1" : "B polls 1";
    PRINTF(b_waits, "%s, %s %s A's priority: B waits", queue, what, where);
    PRINTF(info.currentCount, "%s, %s %s A's priority: the count", queue, what, where);
    PRINTF(info.numWaitThreads, "%s, %s %s A's priority: the waiters", queue, what, where);
    sceKernelCancelSema(sema, 0, NULL);
    test_join_thread(a);
    test_join_thread(b);
    PRINTF(a_result, "%s, %s %s A's priority: A's result", queue, what, where);
    PRINTF(b_result, "%s, %s %s A's priority: B's result", queue, what, where);
    sceKernelDeleteSema(sema);
}

int main(void) {
    static const struct {
        SceUInt32 attr;
        const char *name;
    } queues[] = { { 0, "FIFO queue" }, { SCE_KERNEL_ATTR_THREAD_PRIO, "priority queue" } };
    static const struct {
        int priority;
        const char *where;
    } priorities[] = { { A_PRIORITY - 0x10, "above" }, { A_PRIORITY, "at" }, { A_PRIORITY + 0x10, "below" } };
    for (unsigned int q = 0; q < sizeof(queues) / sizeof(queues[0]); q++)
        for (int kind = WAIT; kind <= POLL; kind++)
            for (unsigned int p = 0; p < sizeof(priorities) / sizeof(priorities[0]); p++)
                run_case(queues[q].attr, queues[q].name, kind, priorities[p].priority, priorities[p].where);

    test_exit();
    return 0;
}
