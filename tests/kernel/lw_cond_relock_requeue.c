// Lightweight condition waiters that were signaled while another thread holds the lightweight
// mutex, and wait to lock it again, when their priority changes. The mutex and the condition have
// SCE_KERNEL_ATTR_THREAD_PRIO unless the case says otherwise. Each condition waiter locks the
// mutex, waits on the condition, and once its wait returns records its turn and unlocks. The main
// thread then holds the mutex and moves the waiters to it with sceKernelSignalLwCondTo, one at a
// time, so they queue for the mutex in the order they were signaled. Cases:
// - one waiter A: the wait type and wait ID it reports while it waits for the mutex, and the
//   waiters each object counts, before and after its priority drops
// - waiters A, B and C at falling priorities, then A drops below C
// - a thread L that locks the mutex the plain way, then A, signaled after L waits but of a higher
//   priority, then A drops below L
// - a FIFO mutex: A, B and C at falling priorities, then A drops below C
// Then the main thread unlocks the mutex once, and the waiters hand it on to each other. Each case
// prints the turn each thread got the mutex in.
//
// On a PS Vita TV on firmware 3.65 a waiter on the lightweight condition reports wait type 0x200000
// with the condition's UID. Once signaled, while it waits for the lightweight mutex, it reports
// 0x400000 with the mutex's UID, and the mutex counts it as a waiter and the condition doesn't,
// also after its priority drops. The drop moves it in the mutex's queue, so the turns are B, C, A,
// and L before A. The FIFO mutex keeps A, B, C.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define LOW 0x98

static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
static SceUID threads[3];
static volatile int result[3], turn[3], turns;

static int cond_waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    sceKernelLockLwMutex(&lw_mutex, 1, NULL);
    SceUInt timeout = 2 * TEST_WAIT_US;
    result[i] = sceKernelWaitLwCond(&lw_cond, &timeout);
    turn[i] = ++turns;
    sceKernelUnlockLwMutex(&lw_mutex, 1);
    return 0;
}

static int locker(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    result[i] = sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
    turn[i] = ++turns;
    if (result[i] == 0)
        sceKernelUnlockLwMutex(&lw_mutex, 1);
    return 0;
}

static SceUInt32 wait_type(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (sceKernelGetThreadInfo(thread, &info) < 0)
        return 0;
    return info.waitType & ~WAIT_TYPE_CB;
}

static SceUID wait_id(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (sceKernelGetThreadInfo(thread, &info) < 0)
        return 0;
    return info.waitId;
}

static LwMutexInfo mutex_info(void) {
    LwMutexInfo info = { .size = sizeof(info) };
    sceKernelGetLwMutexInfo(&lw_mutex, &info);
    return info;
}

static LwCondInfo cond_info(void) {
    LwCondInfo info = { .size = sizeof(info) };
    sceKernelGetLwCondInfo(&lw_cond, &info);
    return info;
}

static void create(SceUInt32 mutex_attr) {
    sceKernelCreateLwMutex(&lw_mutex, "lw mutex", mutex_attr, 0, NULL);
    sceKernelCreateLwCond(&lw_cond, "lw cond", SCE_KERNEL_ATTR_THREAD_PRIO, &lw_mutex, NULL);
    turns = 0;
    for (int i = 0; i < 3; i++) {
        result[i] = 0xDEAD;
        turn[i] = 0;
    }
}

static SceUID start(SceKernelThreadEntry entry, int i, int priority) {
    const SceUID thread = sceKernelCreateThread("waiter", entry, priority, 0x4000, 0, 0, NULL);
    sceKernelStartThread(thread, sizeof(i), &i);
    test_wait_until_waiting(thread);
    return thread;
}

// Returns whether thread waits, and not on the condition
static int in_mutex_wait(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && (info.status & SCE_THREAD_WAITING) &&
           info.waitType != 0 && (info.waitType & ~WAIT_TYPE_CB) != WAIT_TYPE_LW_COND;
}

// Moves condition waiter i to the mutex, which this thread holds
static void signal_to(int i) {
    sceKernelSignalLwCondTo(&lw_cond, threads[i]);
    TEST_WAIT_FOR(in_mutex_wait(threads[i]));
}

// Unlocks the mutex, waits for every thread to have had its turn, and prints the turns
static void finish(const char *name, const char *const *names, int n) {
    sceKernelUnlockLwMutex(&lw_mutex, 1);
    TEST_WAIT_FOR(turns == n);
    for (int i = 0; i < n; i++)
        test_join_thread(threads[i]);
    for (int i = 0; i < n; i++) {
        PRINTF(turn[i], "%s: %s's turn", name, names[i]);
        PRINTF(result[i], "%s: %s's result", name, names[i]);
    }
    sceKernelDeleteLwCond(&lw_cond);
    sceKernelDeleteLwMutex(&lw_mutex);
}

static void print_state(const char *name, const char *when) {
    PRINTF(wait_type(threads[0]), "%s, %s: A's wait type", name, when);
    const SceUID id = wait_id(threads[0]);
    const LwMutexInfo mutex = mutex_info();
    const LwCondInfo cond = cond_info();
    PRINTF(id == cond.uid, "%s, %s: A's wait ID is the condition", name, when);
    PRINTF(id == mutex.uid, "%s, %s: A's wait ID is the mutex", name, when);
    PRINTF(mutex.numWaitThreads, "%s, %s: the mutex's waiters", name, when);
    PRINTF(cond.numWaitThreads, "%s, %s: the condition's waiters", name, when);
}

static void three_waiters(const char *name, SceUInt32 mutex_attr) {
    static const char *const names[3] = { "A", "B", "C" };
    create(mutex_attr);
    for (int i = 0; i < 3; i++)
        threads[i] = start(cond_waiter, i, 0x80 + 8 * i);
    sceKernelLockLwMutex(&lw_mutex, 1, NULL);
    for (int i = 0; i < 3; i++)
        signal_to(i);
    PRINTF(sceKernelChangeThreadPriority(threads[0], LOW), "%s: A drops below C", name);
    finish(name, names, 3);
}

int main(void) {
    const SceUInt32 prio = SCE_KERNEL_ATTR_THREAD_PRIO;

    const char *name = "one waiter";
    static const char *const one[1] = { "A" };
    create(prio);
    threads[0] = start(cond_waiter, 0, 0x80);
    print_state(name, "waiting on the condition");
    sceKernelLockLwMutex(&lw_mutex, 1, NULL);
    signal_to(0);
    print_state(name, "signaled");
    PRINTF(sceKernelChangeThreadPriority(threads[0], LOW), "%s: A drops", name);
    print_state(name, "after the drop");
    finish(name, one, 1);

    three_waiters("A, B and C", prio);

    name = "A and a plain locker L";
    static const char *const a_l[2] = { "A", "L" };
    create(prio);
    threads[0] = start(cond_waiter, 0, 0x80);
    sceKernelLockLwMutex(&lw_mutex, 1, NULL);
    threads[1] = start(locker, 1, 0x88);
    signal_to(0);
    PRINTF(mutex_info().numWaitThreads, "%s: the mutex's waiters", name);
    PRINTF(sceKernelChangeThreadPriority(threads[0], LOW), "%s: A drops below L", name);
    finish(name, a_l, 2);

    three_waiters("FIFO mutex, A, B and C", 0);

    test_exit();
    return 0;
}
