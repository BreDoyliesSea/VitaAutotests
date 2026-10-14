// Whether a priority change makes another thread on the same core run before the call returns:
// the main thread raises a ready thread above itself, lowers itself below one, changes itself to
// the priority of one, and changes itself to its own priority with one ready at it. Also what both
// change calls return and what the current priority reads after each. Every thread is pinned to
// core 0. The threads record their steps as small numbers, printed in the order they were
// recorded: the main thread records 0 before the change and 2 after it, the other thread records 1
// when it runs.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x80
#define OTHER_PRIORITY 0xA0
#define MAX_STEPS 4

int sceKernelChangeThreadPriority2(SceUID thid, int priority);

static int step_count, steps[MAX_STEPS];

static void record(int step) {
    const int i = __atomic_fetch_add(&step_count, 1, __ATOMIC_SEQ_CST);
    if (i < MAX_STEPS)
        steps[i] = step;
}

static int other_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(1);
    return 0;
}

// Starts the other thread, which can't run yet since the main thread is above it or at its priority
static SceUID start_other(int priority) {
    step_count = 0;
    for (int i = 0; i < MAX_STEPS; i++)
        steps[i] = -1;
    const SceUID thread = sceKernelCreateThread("other", other_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    return thread;
}

// Lets the other thread finish, puts the main thread's priority back and prints the steps
static void finish(const char *name, SceUID thread, int result, int priority_after) {
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    sceKernelDeleteThread(thread);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    PRINTF(result, "%s: the change's result", name);
    PRINTF(priority_after, "%s: the current priority right after it", name);
    const int count = step_count < MAX_STEPS ? step_count : MAX_STEPS;
    for (int i = 0; i < count; i++)
        PRINTF(steps[i], "%s: step %d", name, i);
}

static void raise_other(const char *name, int use_2) {
    const SceUID thread = start_other(OTHER_PRIORITY);
    record(0);
    const int result = use_2 ? sceKernelChangeThreadPriority2(thread, MAIN_PRIORITY - 0x10) : sceKernelChangeThreadPriority(thread, MAIN_PRIORITY - 0x10);
    record(2);
    finish(name, thread, result, sceKernelGetThreadCurrentPriority());
}

static void change_self(const char *name, int other_priority, int priority, int use_2) {
    const SceUID thread = start_other(other_priority);
    record(0);
    const int result = use_2 ? sceKernelChangeThreadPriority2(0, priority) : sceKernelChangeThreadPriority(0, priority);
    const int after = sceKernelGetThreadCurrentPriority();
    record(2);
    finish(name, thread, result, after);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);
    PRINT("the main thread's priority", sceKernelGetThreadCurrentPriority());

    raise_other("raise a ready thread above the caller", 0);
    raise_other("raise a ready thread above the caller, the second call", 1);
    change_self("lower the caller below a ready thread", OTHER_PRIORITY, OTHER_PRIORITY + 0x10, 0);
    change_self("lower the caller below a ready thread, the second call", OTHER_PRIORITY, OTHER_PRIORITY + 0x10, 1);
    change_self("change the caller to a ready thread's priority", OTHER_PRIORITY, OTHER_PRIORITY, 0);
    change_self("change the caller to a ready thread's priority, the second call", OTHER_PRIORITY, OTHER_PRIORITY, 1);
    change_self("change the caller to its own priority, with a ready thread at it", MAIN_PRIORITY, MAIN_PRIORITY, 0);
    change_self("change the caller to its own priority, with a ready thread at it, the second call", MAIN_PRIORITY, MAIN_PRIORITY, 1);

    test_exit();
    return 0;
}
