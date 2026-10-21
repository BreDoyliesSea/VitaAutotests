// What sceKernelGetThreadInfo reports as the wait type and the wait UID of a thread that doesn't
// wait, and of a thread right after one of its waits ended. The wait UID is printed as comparisons,
// whether it is -1, 0, the thread's own UID or the UID of the object it waited on. Each thread is
// read first and the values printed later, since printing lets the other threads run. The cases:
// - the running thread, read through UID 0 and through its own UID, and again right after its own
//   semaphore wait timed out
// - a thread that was created and not started
// - a thread that ended, read after sceKernelWaitThreadEnd and before it is deleted
// - a thread that was started and is ready but can't run, since it is pinned to core 0 below the
//   main thread, which doesn't block until the thread was read
// - a thread whose semaphore wait was ended by a signal, read before it ran, then once it waits on
//   a second semaphore, then once a signal of the second one ended that wait and it is ready again
//   without having run
// - a thread whose semaphore wait timed out, read while it spins on core 1, where it doesn't wait
// - a thread that runs its callback inside a wait with callbacks, which reads its own thread info
//   through UID 0 from inside the callback
//
// On a PS Vita TV on firmware 3.65 every thread that isn't waiting reports the wait type 0 and the
// wait UID -1, whether it runs, was never started, ended, is ready after a start or a wake, spins
// after a timeout, or runs a callback inside a wait. A thread that a signal woke reports the ready
// status 0x2 until it runs. A thread waiting on the second semaphore reports that semaphore's UID.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
// Lower than the main thread's
#define LOW_PRIORITY 0xB0
#define NOT_DONE 0xDEAD
// Every wait a case expects to end gets this timeout, which is far past the checks
#define LONG_US (3 * TEST_WAIT_US)

typedef struct State {
    int result, status;
    SceUInt32 wait_type;
    SceUID wait_id;
} State;

static SceUID sema, sema2, callback_id;
static volatile int first_result, second_result, wait_result, spin_stop, callback_ran;
static State callback_state;

// Reads what sceKernelGetThreadInfo reports for thread
static State read_state(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    const int result = sceKernelGetThreadInfo(thread, &info);
    return (State){ .result = result, .status = info.status, .wait_type = info.waitType, .wait_id = info.waitId };
}

// Prints a state read for thread, with the status unless show_status is 0, and whether its wait UID
// is the object, if the case has one
static void print_state(const char *name, State state, SceUID thread, int show_status, const char *object_name, SceUID object) {
    PRINTF(state.result, "%s: the info call", name);
    if (show_status)
        PRINTF(state.status, "%s: its status", name);
    PRINTF(state.wait_type, "%s: its wait type", name);
    PRINTF(state.wait_id == -1, "%s: its wait UID is -1", name);
    PRINTF(state.wait_id == 0, "%s: its wait UID is 0", name);
    PRINTF(state.wait_id == thread, "%s: its wait UID is its own UID", name);
    if (object_name)
        PRINTF(state.wait_id == object, "%s: its wait UID is %s", name, object_name);
}

static SceUID create(SceKernelThreadEntry entry, int priority, SceUInt32 cpu_mask) {
    return sceKernelCreateThread("thread", entry, priority, 0x4000, 0, cpu_mask, NULL);
}

// Waits for thread to end and returns what the wait returned
static int end_wait(SceUID thread) {
    SceUInt timeout = LONG_US;
    return sceKernelWaitThreadEnd(thread, NULL, &timeout);
}

static int idle_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

// Waits for the semaphore, which the main thread signals
static int ended_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = LONG_US;
    first_result = sceKernelWaitSema(sema, 1, &timeout);
    return 0;
}

// Waits for the first semaphore and then for the second
static int two_waits_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = LONG_US;
    first_result = sceKernelWaitSema(sema, 1, &timeout);
    timeout = LONG_US;
    second_result = sceKernelWaitSema(sema2, 1, &timeout);
    return 0;
}

// Waits for the empty semaphore until it times out, then spins until the main thread stops it
static int spin_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 50000;
    first_result = sceKernelWaitSema(sema, 1, &timeout);
    wait_result = 1;
    // Plain spinning, which is no wait. The time bound only keeps a wrong run from hanging.
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    while (!spin_stop && sceKernelGetProcessTimeWide() - start < 3 * (SceUInt64)TEST_WAIT_US)
        ;
    return 0;
}

// Reads its own thread info through UID 0 while it runs the callback of its wait
static int info_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_state = read_state(0);
    __sync_synchronize();
    callback_ran = 1;
    return 0;
}

// Creates a callback and waits for the semaphore in a wait that checks callbacks
static int callback_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback_id = sceKernelCreateCallback("callback", 0, info_callback, NULL);
    SceUInt timeout = LONG_US;
    first_result = sceKernelWaitSemaCB(sema, 1, &timeout);
    sceKernelDeleteCallback(callback_id);
    return 0;
}

static void running_case(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const State via_zero = read_state(0);
    const State via_own_uid = read_state(sceKernelGetThreadId());
    SceUInt timeout = 20000;
    const int result = sceKernelWaitSema(sema, 1, &timeout);
    const State after_timeout = read_state(0);

    print_state("the running thread through UID 0", via_zero, sceKernelGetThreadId(), 1, NULL, 0);
    print_state("the running thread through its own UID", via_own_uid, sceKernelGetThreadId(), 1, NULL, 0);
    PRINTF(result, "the running thread: its own semaphore wait");
    print_state("the running thread after its own wait timed out", after_timeout, sceKernelGetThreadId(), 1, "the semaphore", sema);
    sceKernelDeleteSema(sema);
}

static void dormant_case(void) {
    const SceUID thread = create(idle_entry, LOW_PRIORITY, SCE_KERNEL_CPU_MASK_USER_0);
    const State state = read_state(thread);
    print_state("a thread created and not started", state, thread, 1, NULL, 0);
    sceKernelDeleteThread(thread);
}

static void ended_case(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    first_result = NOT_DONE;
    const SceUID thread = create(ended_entry, LOW_PRIORITY, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema));
    sceKernelSignalSema(sema, 1);
    const int end = end_wait(thread);
    const State state = read_state(thread);

    PRINTF(end, "a thread that ended: the wait for its end");
    PRINTF(first_result, "a thread that ended: its semaphore wait");
    print_state("a thread that ended", state, thread, 1, "the semaphore", sema);
    sceKernelDeleteThread(thread);
    sceKernelDeleteSema(sema);
}

static void ready_case(void) {
    const SceUID thread = create(idle_entry, LOW_PRIORITY, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelStartThread(thread, 0, NULL);
    // The thread can't run before the main thread blocks
    const State state = read_state(thread);
    const int end = end_wait(thread);

    print_state("a started thread that hasn't run", state, thread, 1, NULL, 0);
    PRINTF(end, "a started thread that hasn't run: the wait for its end");
    sceKernelDeleteThread(thread);
}

static void signaled_case(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    sema2 = sceKernelCreateSema("sema2", 0, 0, 1, NULL);
    first_result = second_result = NOT_DONE;
    const SceUID thread = create(two_waits_entry, LOW_PRIORITY, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema));
    sceKernelSignalSema(sema, 1);
    // Woken, ready, and not run, since the main thread has the core
    const State woken = read_state(thread);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema2));
    const State second_wait = read_state(thread);
    sceKernelSignalSema(sema2, 1);
    const State woken_again = read_state(thread);
    const int end = end_wait(thread);

    print_state("first semaphore signaled, thread not run yet", woken, thread, 1, "the first semaphore", sema);
    print_state("thread waits for the second semaphore", second_wait, thread, 1, "the second semaphore", sema2);
    PRINTF(second_wait.wait_id == sema, "thread waits for the second semaphore: its wait UID is the first semaphore");
    print_state("second semaphore signaled, thread not run yet", woken_again, thread, 1, "the second semaphore", sema2);
    PRINTF(end, "signaled thread: the wait for its end");
    PRINTF(first_result, "signaled thread: the first wait");
    PRINTF(second_result, "signaled thread: the second wait");
    sceKernelDeleteThread(thread);
    sceKernelDeleteSema(sema);
    sceKernelDeleteSema(sema2);
}

static void timeout_case(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    first_result = NOT_DONE;
    wait_result = spin_stop = 0;
    const SceUID thread = create(spin_entry, MAIN_PRIORITY, SCE_KERNEL_CPU_MASK_USER_1);
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(wait_result);
    // The thread spins on core 1, which doesn't change what it reports about waiting
    const State state = read_state(thread);
    spin_stop = 1;
    const int end = end_wait(thread);

    PRINTF(first_result, "a wait that timed out: the semaphore wait");
    print_state("a wait that timed out, thread spinning", state, thread, 0, "the semaphore", sema);
    PRINTF(end, "a wait that timed out: the wait for the thread's end");
    sceKernelDeleteThread(thread);
    sceKernelDeleteSema(sema);
}

static void callback_case(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    first_result = NOT_DONE;
    callback_ran = 0;
    callback_id = 0;
    const SceUID thread = create(callback_entry, LOW_PRIORITY, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA | WAIT_TYPE_CB, sema));
    const int notify = sceKernelNotifyCallback(callback_id, 1);
    TEST_WAIT_FOR(callback_ran);
    __sync_synchronize();
    const State state = callback_state;
    sceKernelSignalSema(sema, 1);
    const int end = end_wait(thread);

    PRINTF(notify, "a callback in a wait: the notify");
    PRINTF(callback_ran, "a callback in a wait: the callback ran");
    print_state("a callback in a wait, read from the callback through UID 0", state, thread, 1, "the semaphore", sema);
    PRINTF(first_result, "a callback in a wait: the wait's result");
    PRINTF(end, "a callback in a wait: the wait for the thread's end");
    sceKernelDeleteThread(thread);
    sceKernelDeleteSema(sema);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    running_case();
    dormant_case();
    ended_case();
    ready_case();
    signaled_case();
    timeout_case();
    callback_case();

    test_exit();
    return 0;
}
