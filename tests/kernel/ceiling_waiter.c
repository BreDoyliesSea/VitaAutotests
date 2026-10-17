// What the priority of a thread that waits for a mutex with a ceiling is while it waits, once it
// gets the mutex, and after it unlocks it. Owner O, at 0xB0 on core 0, locks a mutex with the
// ceiling 0x80 and waits on a semaphore. W, on core 0, then waits for the mutex. The main thread,
// on core 1, prints O's and W's current priorities from their info, then signals O. O records 1,
// unlocks, records 3 and notes its priority. W notes its priority once it has the mutex, records
// 2, unlocks and notes its priority again. So 1 2 3 means W took the core from O at the unlock,
// and 1 3 2 that it didn't. The cases:
// - W at 0xB8, below the ceiling and below O
// - W at 0x90, below the ceiling and above O
// - W at 0x60, above the ceiling
// - W at 0xB8, set to 0x90 by the main thread while it waits
// - W at 0xB8, with a timeout of 100 ms that passes before the main thread signals O
//
// On a PS Vita TV on firmware 3.65 a waiter keeps its own priority while it waits for a ceiling
// mutex, so the owner's ceiling doesn't reach it, and gets the ceiling once it holds the mutex,
// unless it is already above it, as at 0x60. After its unlock it has its own priority again,
// including one changed while it waited. The owner runs at the ceiling while W waits and at 0xB0
// after its unlock. The steps go 1 2 3, so W takes the core at the handover. A waiter whose 100 ms
// timeout passes gets 0x80028005 (WAIT_TIMEOUT) and keeps its priority.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define OWNER_PRIORITY 0xB0
#define CEILING 0x80
#define MAX_STEPS 4

static SceUID mutex, sema;
static volatile int w_timeout, w_result, w_priority, w_after, w_done, o_after;
static int step_count, steps[MAX_STEPS];

static void record(int step) {
    const int i = __atomic_fetch_add(&step_count, 1, __ATOMIC_SEQ_CST);
    if (i < MAX_STEPS)
        steps[i] = step;
}

static int o_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelLockMutex(mutex, 1, NULL);
    SceUInt32 timeout = 2 * TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    record(1);
    sceKernelUnlockMutex(mutex, 1);
    record(3);
    o_after = sceKernelGetThreadCurrentPriority();
    return 0;
}

static int w_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = w_timeout ? w_timeout : 2 * TEST_WAIT_US;
    w_result = sceKernelLockMutex(mutex, 1, &timeout);
    w_priority = sceKernelGetThreadCurrentPriority();
    if (w_result == 0) {
        record(2);
        sceKernelUnlockMutex(mutex, 1);
        w_after = sceKernelGetThreadCurrentPriority();
    }
    w_done = 1;
    return 0;
}

static int current_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.currentPriority;
}

// Runs one case, where change is the priority the main thread gives W while it waits, or 0
static void run_case(const char *name, int w_initial, int change, int timeout) {
    step_count = 0;
    for (int i = 0; i < MAX_STEPS; i++)
        steps[i] = -1;
    w_result = w_priority = w_after = o_after = -1;
    w_done = 0;
    w_timeout = timeout;
    SceKernelMutexOptParam opt = { .size = sizeof(opt), .ceilingPriority = CEILING };
    mutex = sceKernelCreateMutex("ceiling", SCE_KERNEL_MUTEX_ATTR_CEILING, 0, &opt);
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID o = sceKernelCreateThread("o", o_entry, OWNER_PRIORITY, 0x4000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_0, NULL);
    const SceUID w = sceKernelCreateThread("w", w_entry, w_initial, 0x4000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(o, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(o, WAIT_TYPE_SEMA, sema));
    sceKernelStartThread(w, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(w, WAIT_TYPE_MUTEX, mutex));
    const int change_result = change ? sceKernelChangeThreadPriority(w, change) : 0;
    const int o_waiting = current_of(o), w_waiting = current_of(w);
    if (timeout)
        TEST_WAIT_FOR(w_done);
    sceKernelSignalSema(sema, 1);
    TEST_WAIT_FOR(step_count == (timeout ? 2 : 3));
    // Joined first, so every record the threads made is complete
    test_join_thread(o);
    test_join_thread(w);
    sceKernelDeleteSema(sema);
    sceKernelDeleteMutex(mutex);

    if (change)
        PRINTF(change_result, "%s: the change", name);
    PRINTF(o_waiting, "%s: O's priority while W waits", name);
    PRINTF(w_waiting, "%s: W's priority while it waits", name);
    PRINTF(w_result, "%s: W's lock", name);
    PRINTF(w_priority, "%s: W's priority after its lock", name);
    if (!timeout)
        PRINTF(w_after, "%s: W's priority after its unlock", name);
    PRINTF(o_after, "%s: O's priority after its unlock", name);
    for (int i = 0; i < (timeout ? 2 : 3); i++)
        PRINTF(steps[i], "%s: step %d", name, i);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case("W at 0xB8", 0xB8, 0, 0);
    run_case("W at 0x90", 0x90, 0, 0);
    run_case("W at 0x60", 0x60, 0, 0);
    run_case("W at 0xB8 set to 0x90 while it waits", 0xB8, 0x90, 0);
    run_case("W at 0xB8 with a timeout of 100 ms", 0xB8, 0, 100000);
    test_exit();
    return 0;
}
