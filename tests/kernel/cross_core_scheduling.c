// Scheduling across cores 0 and 1, with the main thread on core 2. Which of two running threads
// pinned to cores 0 and 1 a woken thread that may use both takes the core from: with the running
// threads at different priorities either way round and at the same priority, for a woken thread
// at 0x90 and at 0x70. Whether a thread with the default CPU affinity mask runs on core 2 while
// cores 0 and 1 are busy. What a thread at 0x70 woken while both its cores are busy does when the
// core it didn't last run on frees up, as it is, after its priority was changed to 0xA0, or after
// its mask was changed to that core alone. A thread that never ran started while both cores are
// busy, and whether it runs once one or both of them free up. And in which order three threads at
// 0xA0 made ready while both cores are busy run as the cores free up.
// The main thread runs above every test thread. The output never names a core, only which of the
// test's threads ran, as bits: 1 for spinner 0, 2 for spinner 1, 4 for the woken thread T.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define BLOCKER_PRIORITY 0x48
#define BOTH_CORES (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1)
#define SPINS 400000000

static SceUID sema;
static volatile int stop_spin[2], spinning[2];
static volatile unsigned int spun[2];
static volatile int stop_t, t_waits;
static volatile unsigned int t_counter;

// Spinner i runs on core i until told to stop, counting as it goes
static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    spinning[i] = 1;
    for (int n = 0; n < SPINS && !stop_spin[i]; n++)
        spun[i]++;
    return 0;
}

static SceUID spinners[2];

static void start_spinner(int i, int priority) {
    stop_spin[i] = spinning[i] = 0;
    spun[i] = 0;
    spinners[i] = sceKernelCreateThread("spinner", spinner_entry, priority, 0x4000, 0, i == 0 ? SCE_KERNEL_CPU_MASK_USER_0 : SCE_KERNEL_CPU_MASK_USER_1, NULL);
    sceKernelStartThread(spinners[i], sizeof(i), &i);
    TEST_WAIT_FOR(spinning[i]);
}

static void stop_spinner(int i) {
    stop_spin[i] = 1;
    sceKernelWaitThreadEnd(spinners[i], NULL, NULL);
    sceKernelDeleteThread(spinners[i]);
}

// Waits on the semaphore, then spins until told to stop
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    t_waits = 1;
    SceUInt timeout = 3 * TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    for (int n = 0; n < SPINS && !stop_t; n++)
        t_counter++;
    return 0;
}

static SceUID start_t(int priority, SceUInt32 mask) {
    stop_t = t_waits = 0;
    t_counter = 0;
    const SceUID t = sceKernelCreateThread("t", t_entry, priority, 0x4000, 0, mask, NULL);
    sceKernelStartThread(t, 0, NULL);
    TEST_WAIT_FOR(t_waits && test_in_wait(t, WAIT_TYPE_SEMA, sema));
    return t;
}

static void end_t(SceUID t) {
    stop_t = 1;
    sceKernelWaitThreadEnd(t, NULL, NULL);
    sceKernelDeleteThread(t);
}

// Which of the spinners and T make progress within 50 ms, as bits: 1 spinner 0, 2 spinner 1, 4 T
static int progress(void) {
    const unsigned int s0 = spun[0], s1 = spun[1], t = t_counter;
    sceKernelDelayThread(50000);
    return (spun[0] != s0) | (spun[1] != s1) << 1 | (t_counter != t) << 2;
}

static int status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

static void preempt_case(int t_priority, int p0, int p1) {
    const SceUID t = start_t(t_priority, BOTH_CORES);
    start_spinner(0, p0);
    start_spinner(1, p1);
    const int before = progress();
    sceKernelSignalSema(sema, 1);
    sceKernelDelayThread(10000);
    const int during = progress();
    end_t(t);
    stop_spinner(0);
    stop_spinner(1);
    PRINTF(before, "T at 0x%X woken while spinner 0 runs at 0x%X and spinner 1 at 0x%X: what ran before", t_priority, p0, p1);
    if (p0 == p1) {
        // Which of two equal spinners stops isn't fixed, only that one does
        const int one_stopped = (during & 4) && ((during & 3) == 1 || (during & 3) == 2);
        PRINTF(one_stopped, "T at 0x%X woken while both spinners run at 0x%X: T and one spinner ran after", t_priority, p0);
    } else {
        PRINTF(during, "T at 0x%X woken while spinner 0 runs at 0x%X and spinner 1 at 0x%X: what ran after", t_priority, p0, p1);
    }
}

static void default_mask_case(void) {
    start_spinner(0, BLOCKER_PRIORITY);
    start_spinner(1, BLOCKER_PRIORITY);
    const SceUID t = start_t(0xA0, SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT);
    sceKernelSignalSema(sema, 1);
    sceKernelDelayThread(10000);
    const int during = progress();
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(t, &info);
    end_t(t);
    stop_spinner(0);
    stop_spinner(1);
    PRINT("the default mask, cores 0 and 1 busy: what ran", during);
    PRINT("the default mask, cores 0 and 1 busy: T's last core is core 2", info.lastExecutedCpuId == 2);
}

// T at 0x70 is woken while both of its cores are busy. Then change is applied, and the core T
// didn't last run on frees up, then the one it did.
typedef enum Change { NONE, TO_SHARED_PRIORITY, TO_OTHER_CORE } Change;

static void standby_case(Change change, const char *name) {
    const SceUID t = start_t(0x70, BOTH_CORES);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(t, &info);
    const int last = info.lastExecutedCpuId, other = 1 - last;
    start_spinner(0, BLOCKER_PRIORITY);
    start_spinner(1, BLOCKER_PRIORITY);
    sceKernelSignalSema(sema, 1);
    sceKernelDelayThread(10000);
    const int status_before = status_of(t);
    int result = 0;
    if (change == TO_SHARED_PRIORITY)
        result = sceKernelChangeThreadPriority(t, 0xA0);
    else if (change == TO_OTHER_CORE)
        result = sceKernelChangeThreadCpuAffinityMask(t, other == 0 ? SCE_KERNEL_CPU_MASK_USER_0 : SCE_KERNEL_CPU_MASK_USER_1);
    const int status_after = status_of(t);
    stop_spinner(other);
    sceKernelDelayThread(10000);
    const int runs_other = (progress() & 4) != 0;
    stop_spinner(last);
    sceKernelDelayThread(10000);
    const int runs_last = (progress() & 4) != 0;
    end_t(t);
    PRINTF(last == 0 || last == 1, "%s: T last ran on core 0 or 1", name);
    PRINTF(status_before, "%s: T's status while both cores are busy", name);
    PRINTF(result >= 0, "%s: the change succeeds", name);
    PRINTF(status_after, "%s: T's status after the change", name);
    PRINTF(runs_other, "%s: the core T didn't last run on frees up: T runs", name);
    PRINTF(runs_last, "%s: then its last core frees up too: T runs", name);
}

static int spin_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (int n = 0; n < SPINS && !stop_t; n++)
        t_counter++;
    return 0;
}

// T at priority, which never ran, is started while both of its cores are busy. Then core first
// frees up, then the other one.
static void start_both_busy(int priority, int first) {
    start_spinner(0, BLOCKER_PRIORITY);
    start_spinner(1, BLOCKER_PRIORITY);
    stop_t = 0;
    t_counter = 0;
    const SceUID t = sceKernelCreateThread("t", spin_entry, priority, 0x4000, 0, BOTH_CORES, NULL);
    sceKernelStartThread(t, 0, NULL);
    sceKernelDelayThread(10000);
    const int status = status_of(t);
    stop_spinner(first);
    sceKernelDelayThread(10000);
    const int runs_first = (progress() & 4) != 0;
    stop_spinner(1 - first);
    sceKernelDelayThread(10000);
    const int runs_both = (progress() & 4) != 0;
    end_t(t);
    PRINTF(status, "T at 0x%X started while both its cores are busy: its status", priority);
    // At 0x40 to 0x7F, which core such a thread waits for changes from run to run
    if (priority >= 0x80)
        PRINTF(runs_first, "T at 0x%X started while both its cores are busy, then core %d frees up: T runs", priority, first);
    PRINTF(runs_both, "T at 0x%X started while both its cores are busy, then the other core too: T runs", priority);
}

static volatile int ran_count;
static int ran_order[3];

static int order_entry(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    const int n = __atomic_fetch_add(&ran_count, 1, __ATOMIC_SEQ_CST);
    if (n < 3)
        ran_order[n] = i;
    return 0;
}

static void shared_fifo_case(void) {
    start_spinner(0, BLOCKER_PRIORITY);
    start_spinner(1, BLOCKER_PRIORITY);
    ran_count = 0;
    SceUID threads[3];
    for (int i = 0; i < 3; i++) {
        ran_order[i] = 0xF;
        threads[i] = sceKernelCreateThread("order", order_entry, 0xA0, 0x4000, 0, BOTH_CORES, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
    }
    stop_spinner(0);
    TEST_WAIT_FOR(ran_count == 3);
    stop_spinner(1);
    for (int i = 0; i < 3; i++)
        test_join_thread(threads[i]);
    PRINT("three threads at 0xA0 started while both cores are busy, then core 0 frees up: run order", (ran_order[0] << 8) | (ran_order[1] << 4) | ran_order[2]);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    // Above every test thread, so it gets core 2 back from one running there
    sceKernelChangeThreadPriority(0, 0x60);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);

    preempt_case(0x90, 0xB0, 0xA0);
    preempt_case(0x90, 0xA0, 0xB0);
    preempt_case(0x90, 0xA0, 0xA0);
    preempt_case(0x70, 0xB0, 0xA0);
    preempt_case(0x70, 0xA0, 0xB0);
    preempt_case(0x70, 0xA0, 0xA0);
    preempt_case(0x70, 0x78, 0xA0);
    preempt_case(0x70, 0xA0, 0x78);
    default_mask_case();
    standby_case(NONE, "no change");
    standby_case(TO_SHARED_PRIORITY, "priority changed to 0xA0");
    standby_case(TO_OTHER_CORE, "mask changed to the other core");
    start_both_busy(0x70, 0);
    start_both_busy(0x70, 1);
    start_both_busy(0xA0, 0);
    start_both_busy(0xA0, 1);
    shared_fifo_case();

    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}
