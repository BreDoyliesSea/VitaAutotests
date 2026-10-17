// Whether a priority change that moves semaphore waiters around wakes the ones that now fit,
// without any signal. Waiter A needs 3 and queues first, the waiters behind it need 1 or 3, and the
// semaphore holds 2, which a waiter behind A doesn't get while A waits at the head
// (sema_head_leaves). Then one priority change:
// - priority queue, B needs 1 and is raised above A
// - priority queue, A and B at the same priority, A set to the priority it has
// - priority queue, B and C need 1, A drops below both
// - priority queue, B needs 3 and C needs 1, A drops below both, then C is raised above B
// - FIFO queue, B needs 1: A drops below B, B is raised above A, or A is set to the priority it
//   has, with B at the same priority
// Each case prints how many waiters were done before the change, the count right after it, and
// which waiters got their count within 500 ms of it. The waiters that don't get it end when the
// semaphore is deleted, long before their own timeouts.
//
// On a PS Vita TV on firmware 3.65 the change itself wakes the waiters that now fit, with no
// signal, in a priority queue: B raised above A, or A set to its own priority behind an equal B,
// gets B its count at once, and A dropping below B and C gets both theirs. The wake stops at the
// first waiter that doesn't fit, so A dropping below a B that needs 3 wakes no one, and raising C
// above B then wakes C. A FIFO queue wakes no one. The waits the change leaves end with WAIT_DELETE
// (0x80028006).
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define HIGH 0x78
#define A_PRIORITY 0x80
#define B_PRIORITY 0x88
#define C_PRIORITY 0x90
#define LOW 0x98

static SceUID sema;
static SceUID threads[3];
static int need[3];
static volatile int result[3], done[3];
static int count;

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    result[i] = sceKernelWaitSema(sema, need[i], &timeout);
    done[i] = 1;
    return 0;
}

static int current_count(void) {
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(sema, &info);
    return info.currentCount;
}

// Creates the semaphore with 2 and starts the waiters in order, each once the one before waits
static void start(SceUInt32 attr, const int *needs, const int *priorities, int n) {
    sema = sceKernelCreateSema("sema", attr, 2, 10, NULL);
    count = n;
    for (int i = 0; i < 3; i++)
        done[i] = 0;
    for (int i = 0; i < n; i++) {
        need[i] = needs[i];
        result[i] = 0xDEAD;
        threads[i] = sceKernelCreateThread("waiter", waiter, priorities[i], 0x4000, 0, 0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }
}

// Waits up to 500 ms for every waiter but A to be done, for wakes that would come at once
static void settle(void) {
    for (int t = 0; t < 500; t++) {
        int all = 1;
        for (int i = 1; i < count; i++)
            all &= done[i];
        if (all)
            return;
        sceKernelDelayThread(1000);
    }
}

static void print_done(const char *name, const char *when) {
    static const char *const names[] = { "A", "B", "C" };
    for (int i = 0; i < count; i++)
        PRINTF(done[i], "%s, %s: %s got its count", name, when, names[i]);
}

// Deletes the semaphore, which ends the waits left, and prints each waiter's result
static void finish(const char *name) {
    static const char *const names[] = { "A", "B", "C" };
    sceKernelDeleteSema(sema);
    for (int i = 0; i < count; i++)
        test_join_thread(threads[i]);
    for (int i = 0; i < count; i++)
        PRINTF(result[i], "%s: %s's result", name, names[i]);
}

// Changes the priority of one waiter and prints what that led to
static void change_case(const char *name, SceUInt32 attr, const int *needs, const int *priorities,
                        int n, int who, int priority) {
    start(attr, needs, priorities, n);
    const int before = done[1] + done[2];
    const int changed = sceKernelChangeThreadPriority(threads[who], priority);
    const int after = current_count();
    settle();
    PRINTF(before, "%s: waiters done before the change", name);
    PRINTF(changed, "%s: the change", name);
    PRINTF(after, "%s: the count right after the change", name);
    print_done(name, "after the change");
    finish(name);
}

int main(void) {
    static const int one_behind[2] = { 3, 1 };
    static const int two_behind[3] = { 3, 1, 1 };
    static const int big_then_small[3] = { 3, 3, 1 };
    static const int a_b[2] = { A_PRIORITY, B_PRIORITY };
    static const int same[2] = { A_PRIORITY, A_PRIORITY };
    static const int a_b_c[3] = { A_PRIORITY, B_PRIORITY, C_PRIORITY };
    const SceUInt32 prio = SCE_KERNEL_ATTR_THREAD_PRIO;

    change_case("priority queue, B raised above A", prio, one_behind, a_b, 2, 1, HIGH);
    change_case("priority queue, A set to its own priority", prio, one_behind, same, 2, 0,
                A_PRIORITY);
    change_case("priority queue, A drops below B and C", prio, two_behind, a_b_c, 3, 0, LOW);
    change_case("FIFO queue, A drops below B", 0, one_behind, a_b, 2, 0, LOW);
    change_case("FIFO queue, B raised above A", 0, one_behind, a_b, 2, 1, HIGH);
    change_case("FIFO queue, A set to its own priority", 0, one_behind, same, 2, 0,
                A_PRIORITY);

    // B needs 3 too, so a wake that stops at the first waiter that doesn't fit leaves C waiting
    const char *name = "priority queue, A drops below B that needs 3";
    start(prio, big_then_small, a_b_c, 3);
    const int before = done[1] + done[2];
    sceKernelChangeThreadPriority(threads[0], LOW);
    int after = current_count();
    settle();
    PRINTF(before, "%s: waiters done before the change", name);
    PRINTF(after, "%s: the count right after the change", name);
    print_done(name, "after the change");
    name = "priority queue, then C raised above B";
    sceKernelChangeThreadPriority(threads[2], HIGH);
    after = current_count();
    settle();
    PRINTF(after, "%s: the count right after the change", name);
    print_done(name, "after the change");
    finish(name);

    test_exit();
    return 0;
}
