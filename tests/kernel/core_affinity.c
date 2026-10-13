// How threads share a core. Busy threads pinned to one core, with the same priority or not,
// and the affinity masks a thread reports.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define WORKERS 3
#define SPINS 20000000
#define PRIO_HIGH 100
#define PRIO_LOW 120
// How long the test waits for the workers before it counts them as stuck
#define DEADLINE_US 10000000

static SceUID go[WORKERS], idle;
static int log_count, log_entries[2 * WORKERS];
// How far each worker has spun, written without a syscall
static volatile int progress[WORKERS];
static int with_syscalls;

// Records that worker i started or ended, as 2 * i or 2 * i + 1
static void record(int entry) {
    log_entries[__atomic_fetch_add(&log_count, 1, __ATOMIC_SEQ_CST)] = entry;
}

// Returns where entry is in the log, or -1 if it isn't there
static int position(int entry) {
    for (int i = 0; i < log_count; i++) {
        if (log_entries[i] == entry)
            return i;
    }
    return -1;
}

static int worker(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    // No timeout, so a worker only starts once it is told to
    sceKernelWaitSema(go[i], 1, NULL);
    record(2 * i);
    for (volatile int n = 0; n < SPINS; n++) {
        if (n % 1000 == 0) {
            progress[i] = n;
            // A poll that fails returns without blocking
            if (with_syscalls)
                sceKernelPollSema(idle, 1);
        }
    }
    record(2 * i + 1);
    return 0;
}

// Waits for a worker to end and deletes it. A worker still running after DEADLINE_US ends the test,
// so it can't log into a later case.
static void join_worker(SceUID thread) {
    SceUInt timeout = DEADLINE_US;
    if (sceKernelWaitThreadEnd(thread, NULL, &timeout) != 0) {
        PRINT("a worker didn't end in time", 1);
        test_exit();
    }
    sceKernelDeleteThread(thread);
}

static SceUID start_worker(int i, int priority, int mask) {
    const SceUID thread = sceKernelCreateThread("worker", worker, priority, 0x4000, 0, mask, NULL);
    sceKernelStartThread(thread, sizeof(i), &i);
    test_wait_until_waiting(thread);
    return thread;
}

// Releases every worker at once and returns 1 if each ran from start to end before the next started
static int run_together(const int masks[WORKERS]) {
    SceUID threads[WORKERS];
    log_count = 0;
    for (int i = 0; i < WORKERS; i++)
        threads[i] = start_worker(i, PRIO_HIGH, masks[i]);
    for (int i = 0; i < WORKERS; i++)
        sceKernelSignalSema(go[i], 1);
    for (int i = 0; i < WORKERS; i++)
        join_worker(threads[i]);
    for (int i = 0; i < WORKERS; i++) {
        if (log_entries[2 * i] % 2 != 0 || log_entries[2 * i + 1] != log_entries[2 * i] + 1)
            return 0;
    }
    return 1;
}

// Starts a worker with first_priority on core 1, then one with second_priority once the first is
// halfway through. Where both spin at once, the first then ends well before the second.
static void run_after(int first_priority, int second_priority) {
    log_count = 0;
    progress[0] = 0;
    const SceUID first = start_worker(0, first_priority, CPU_MASK_USER_1);
    const SceUID second = start_worker(1, second_priority, CPU_MASK_USER_1);
    sceKernelSignalSema(go[0], 1);
    const SceUInt64 deadline = sceKernelGetProcessTimeWide() + DEADLINE_US;
    while (progress[0] < SPINS / 2 && sceKernelGetProcessTimeWide() < deadline)
        sceKernelDelayThread(1000);
    sceKernelSignalSema(go[1], 1);
    join_worker(first);
    join_worker(second);
}

// Returns whether entry a was logged before entry b, which both have to be in the log
static int before(int a, int b) {
    return position(a) >= 0 && position(b) >= 0 && position(a) < position(b);
}

int main(void) {
    for (int i = 0; i < WORKERS; i++)
        go[i] = sceKernelCreateSema("go", 0, 0, 1, NULL);
    idle = sceKernelCreateSema("idle", 0, 0, 1, NULL);

    const SceUID unpinned = sceKernelCreateThread("unpinned", worker, PRIO_HIGH, 0x4000, 0, 0, NULL);
    PRINT("the affinity of a thread created with mask 0", sceKernelGetThreadCpuAffinityMask(unpinned));
    PRINT("change the affinity of a thread created with mask 0", sceKernelChangeThreadCpuAffinityMask(unpinned, CPU_MASK_USER_1));
    sceKernelDeleteThread(unpinned);

    // Keeps the main thread off core 1, where the workers run
    PRINT("change the affinity of the main thread", sceKernelChangeThreadCpuAffinityMask(0, CPU_MASK_USER_0));

    const int one_core[WORKERS] = { CPU_MASK_USER_1, CPU_MASK_USER_1, CPU_MASK_USER_1 };
    const int own_cores[WORKERS] = { CPU_MASK_USER_0, CPU_MASK_USER_1, CPU_MASK_USER_2 };
    PRINT("same priority threads on one core run one after another", run_together(one_core));
    with_syscalls = 1;
    PRINT("they do with syscalls too", run_together(one_core));
    with_syscalls = 0;
    PRINT("threads on their own cores run one after another", run_together(own_cores));

    run_after(PRIO_LOW, PRIO_HIGH);
    PRINT("a higher priority thread ends before the running lower one", before(3, 1));
    run_after(PRIO_HIGH, PRIO_LOW);
    PRINT("a lower priority thread starts after the running higher one ends", before(1, 2));

    test_exit();
    return 0;
}
