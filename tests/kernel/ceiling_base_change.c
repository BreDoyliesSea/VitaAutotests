// What a priority change does to a thread that holds a mutex with a ceiling, which runs it at the
// ceiling while it holds it (mutex_ceiling). Owner O, at 0xB0 on core 0, locks a mutex with the
// ceiling 0x80, notes its priority, and waits on a semaphore, while the main thread, on core 1 at
// 0xA0, may change O's priority. The main thread prints O's current and initial priorities from
// its info while it waits, then signals it. O notes its priority again, starts M at 0x90 on core
// 0, which makes it READY behind O, records 1, unlocks, records 3 and notes its priority. M
// records 2, so 1 2 3 means M took the core at the unlock and 1 3 2 that it didn't. The cases:
// - no change
// - O lowered to 0xB8 from core 1, still below the ceiling
// - O raised to 0x70 from core 1, above the ceiling
// - O's priority set to 0 from core 1, which stands for the caller's priority
// - O sets its own priority to 0x70 once it holds the mutex
// - O sets its own priority to 0 once it holds the mutex
// Then the main thread locks such a mutex itself and prints its own priority, the priority of a
// thread it creates with priority 0, and that of a dormant thread whose priority it sets to 0.
//
// On a PS Vita TV on firmware 3.65 O runs at the ceiling 0x80 while it holds the mutex, and its
// info reports 0x80 as its current priority and 0xB0 as its initial one. A change to a priority
// below the ceiling, 0xB8, or 0 from the main thread, which stands for the main thread's 0xA0,
// leaves O at 0x80 until the unlock, and O gets the new priority then, so M takes the core at the
// unlock, 1 2 3. A change above the ceiling, to 0x70, applies at once and stays after the unlock, 1
// 3 2, from the main thread or from O itself. O setting its own priority to 0 takes the 0x80 it
// runs at, which it keeps after the unlock, 1 3 2. The main thread runs at 0x80 too while it holds
// such a mutex, and priority 0 then means 0x80, for a thread it creates and for a dormant thread it
// changes.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define OWNER_PRIORITY 0xB0
#define MIDDLE_PRIORITY 0x90
#define CEILING 0x80
#define MAX_STEPS 4

static SceUID mutex, sema, m;
static volatile int self_change, locked_priority, self_result, woken_priority, after_priority;
static int step_count, steps[MAX_STEPS];

static void record(int step) {
    const int i = __atomic_fetch_add(&step_count, 1, __ATOMIC_SEQ_CST);
    if (i < MAX_STEPS)
        steps[i] = step;
}

static SceUID create_ceiling(void) {
    SceKernelMutexOptParam opt = { .size = sizeof(opt), .ceilingPriority = CEILING };
    return sceKernelCreateMutex("ceiling", SCE_KERNEL_MUTEX_ATTR_CEILING, 0, &opt);
}

static int owner_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelLockMutex(mutex, 1, NULL);
    locked_priority = sceKernelGetThreadCurrentPriority();
    if (self_change >= 0)
        self_result = sceKernelChangeThreadPriority(0, self_change);
    SceUInt32 timeout = 2 * TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    woken_priority = sceKernelGetThreadCurrentPriority();
    sceKernelStartThread(m, 0, NULL);
    record(1);
    sceKernelUnlockMutex(mutex, 1);
    record(3);
    after_priority = sceKernelGetThreadCurrentPriority();
    return 0;
}

static int m_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(2);
    return 0;
}

// Runs one case, where remote is the priority the main thread gives O and self the one O gives
// itself, or -1 for none
static void run_case(const char *name, int remote, int self) {
    step_count = 0;
    for (int i = 0; i < MAX_STEPS; i++)
        steps[i] = -1;
    locked_priority = self_result = woken_priority = after_priority = -1;
    self_change = self;
    mutex = create_ceiling();
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    m = sceKernelCreateThread("m", m_entry, MIDDLE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0,
                              NULL);
    const SceUID owner = sceKernelCreateThread("owner", owner_entry, OWNER_PRIORITY, 0x4000, 0,
                                               SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(owner, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(owner, WAIT_TYPE_SEMA, sema));
    const int remote_result = remote >= 0 ? sceKernelChangeThreadPriority(owner, remote) : 0;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(owner, &info);
    sceKernelSignalSema(sema, 1);
    TEST_WAIT_FOR(step_count == 3);
    // Joined first, so every record the threads made is complete
    test_join_thread(owner);
    test_join_thread(m);
    sceKernelDeleteSema(sema);
    sceKernelDeleteMutex(mutex);

    if (remote >= 0)
        PRINTF(remote_result, "%s: the change", name);
    PRINTF(locked_priority, "%s: O's priority once it holds the mutex", name);
    if (self >= 0)
        PRINTF(self_result, "%s: the change", name);
    PRINTF(info.currentPriority, "%s: O's current priority in its info while it waits", name);
    PRINTF(info.initPriority, "%s: O's initial priority in its info while it waits", name);
    PRINTF(woken_priority, "%s: O's priority after its wait", name);
    for (int i = 0; i < 3; i++)
        PRINTF(steps[i], "%s: step %d", name, i);
    PRINTF(after_priority, "%s: O's priority after the unlock", name);
}

static int empty_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

static int current_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.currentPriority;
}

static int init_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.initPriority;
}

static void main_holds(void) {
    mutex = create_ceiling();
    const SceUID dormant = sceKernelCreateThread("dormant", empty_entry, OWNER_PRIORITY, 0x4000, 0,
                                                 0, NULL);
    PRINT("the main thread locks a mutex with the ceiling 0x80",
          sceKernelLockMutex(mutex, 1, NULL));
    PRINT("the main thread's priority", sceKernelGetThreadCurrentPriority());
    PRINT("the main thread's current priority in its info", current_of(0));
    PRINT("the main thread's initial priority in its info", init_of(0));
    const SceUID zero = sceKernelCreateThread("zero", empty_entry, 0, 0x4000, 0, 0, NULL);
    PRINT("create a thread with priority 0", test_created(zero));
    PRINT("its initial priority", zero >= 0 ? init_of(zero) : -1);
    PRINT("set the priority of a dormant thread to 0", sceKernelChangeThreadPriority(dormant, 0));
    PRINT("its current priority", current_of(dormant));
    PRINT("the main thread unlocks", sceKernelUnlockMutex(mutex, 1));
    PRINT("the main thread's priority after the unlock", sceKernelGetThreadCurrentPriority());
    if (zero >= 0)
        sceKernelDeleteThread(zero);
    sceKernelDeleteThread(dormant);
    sceKernelDeleteMutex(mutex);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    PRINT("the main thread's priority", sceKernelGetThreadCurrentPriority());
    run_case("no change", -1, -1);
    run_case("O lowered to 0xB8", 0xB8, -1);
    run_case("O raised to 0x70", 0x70, -1);
    run_case("O set to 0", 0, -1);
    run_case("O sets itself to 0x70", -1, 0x70);
    run_case("O sets itself to 0", -1, 0);
    main_holds();
    test_exit();
    return 0;
}
