// Mutexes with SCE_KERNEL_MUTEX_ATTR_CEILING: which option structs and ceiling priorities their
// create takes, what the mutex info reports as the ceiling, and what the owner's priority is while
// it holds one: below the ceiling, above it, for two nested ones, with a ceiling in 0x40 to 0x7F
// for an owner in 0x80 to 0xBF, and after a priority change while it holds one. Also whether a
// middle priority thread on the owner's core runs before the owner unlocks, and the same for a
// rwlock created with the attribute bit 0x4. The test threads run on core 0 and the main thread on
// core 1.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define OWNER_PRIORITY 0xB0
#define MIDDLE_PRIORITY 0x90
#define CEILING 0x80
#define MAX_STEPS 4
#define SPINS 100000000

static SceUID create_ceiling(int priority) {
    SceKernelMutexOptParam opt = { .size = sizeof(opt), .ceilingPriority = priority };
    return sceKernelCreateMutex("ceiling", SCE_KERNEL_MUTEX_ATTR_CEILING, 0, &opt);
}

static int ceiling_of(SceUID mutex) {
    MutexInfo info = { .size = sizeof(info) };
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
    return info.ceilingPriority;
}

static void creates(void) {
    PRINT("create with the attribute and no option struct", test_created(sceKernelCreateMutex("m", SCE_KERNEL_MUTEX_ATTR_CEILING, 0, NULL)));
    SceKernelMutexOptParam small = { .size = 4, .ceilingPriority = CEILING };
    PRINT("create with an option struct of size 4", test_created(sceKernelCreateMutex("m", SCE_KERNEL_MUTEX_ATTR_CEILING, 0, &small)));
    SceKernelMutexOptParam none = { .size = sizeof(none), .ceilingPriority = CEILING };
    const SceUID plain = sceKernelCreateMutex("m", 0, 0, &none);
    PRINT("create without the attribute but with a ceiling", test_created(plain));
    PRINT("without the attribute: the ceiling in its info", plain >= 0 ? ceiling_of(plain) : -1);
    if (plain >= 0)
        sceKernelDeleteMutex(plain);
    static const int ceilings[] = { 0, SCE_KERNEL_HIGHEST_PRIORITY_USER - 1, SCE_KERNEL_HIGHEST_PRIORITY_USER, 0x7F, 0x80, SCE_KERNEL_LOWEST_PRIORITY_USER, SCE_KERNEL_LOWEST_PRIORITY_USER + 1, SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_DEFAULT_PRIORITY_USER - 0x20 };
    for (unsigned i = 0; i < sizeof(ceilings) / sizeof(ceilings[0]); i++) {
        const SceUID mutex = create_ceiling(ceilings[i]);
        PRINTF(test_created(mutex), "create with the ceiling 0x%X", ceilings[i]);
        if (mutex >= 0) {
            PRINTF(ceiling_of(mutex), "the ceiling 0x%X: the ceiling in its info", ceilings[i]);
            sceKernelDeleteMutex(mutex);
        }
    }
}

static SceUID mutexes[2];
static volatile int owner_priority[6];

// Runs at OWNER_PRIORITY and records its current priority at each step
static int owner_entry(SceSize args, void *argp) {
    (void)args;
    const int nested = *(const int *)argp;
    owner_priority[0] = sceKernelGetThreadCurrentPriority();
    owner_priority[1] = sceKernelLockMutex(mutexes[0], 1, NULL);
    owner_priority[2] = sceKernelGetThreadCurrentPriority();
    if (nested) {
        sceKernelLockMutex(mutexes[1], 1, NULL);
        owner_priority[3] = sceKernelGetThreadCurrentPriority();
        sceKernelUnlockMutex(mutexes[1], 1);
        owner_priority[4] = sceKernelGetThreadCurrentPriority();
    }
    sceKernelUnlockMutex(mutexes[0], 1);
    owner_priority[5] = sceKernelGetThreadCurrentPriority();
    return 0;
}

static void owner_case(const char *name, int ceiling, int ceiling2, int priority) {
    mutexes[0] = create_ceiling(ceiling);
    mutexes[1] = ceiling2 ? create_ceiling(ceiling2) : -1;
    for (int i = 0; i < 6; i++)
        owner_priority[i] = -1;
    int nested = ceiling2 != 0;
    const SceUID owner = sceKernelCreateThread("owner", owner_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(owner, sizeof(nested), &nested);
    test_join_thread(owner);
    PRINTF(owner_priority[0], "%s: the priority before the lock", name);
    PRINTF(owner_priority[1], "%s: the lock", name);
    PRINTF(owner_priority[2], "%s: the priority while it holds it", name);
    if (nested) {
        PRINTF(owner_priority[3], "%s: the priority while it holds both", name);
        PRINTF(owner_priority[4], "%s: the priority after unlocking the inner one", name);
    }
    PRINTF(owner_priority[5], "%s: the priority after the unlock", name);
    sceKernelDeleteMutex(mutexes[0]);
    if (nested)
        sceKernelDeleteMutex(mutexes[1]);
}

static volatile int changed[4];

static int change_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelLockMutex(mutexes[0], 1, NULL);
    changed[0] = sceKernelChangeThreadPriority(0, 0xA8);
    changed[1] = sceKernelGetThreadCurrentPriority();
    sceKernelUnlockMutex(mutexes[0], 1);
    changed[2] = sceKernelGetThreadCurrentPriority();
    return 0;
}

static void change_case(void) {
    mutexes[0] = create_ceiling(CEILING);
    const SceUID thread = sceKernelCreateThread("owner", change_entry, OWNER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    test_join_thread(thread);
    PRINT("a priority change while it holds the mutex: the change", changed[0]);
    PRINT("a priority change while it holds the mutex: the priority after it", changed[1]);
    PRINT("a priority change while it holds the mutex: the priority after the unlock", changed[2]);
    sceKernelDeleteMutex(mutexes[0]);
}

// Owner L holds the lock at OWNER_PRIORITY on core 0, and M at MIDDLE_PRIORITY becomes ready there
static SceUID lock;
static volatile int is_rwlock, l_locked, l_go;
static int step_count, steps[MAX_STEPS];

static void record(int step) {
    const int i = __atomic_fetch_add(&step_count, 1, __ATOMIC_SEQ_CST);
    if (i < MAX_STEPS)
        steps[i] = step;
}

static int l_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    if (is_rwlock)
        sceKernelLockWriteRWLock(lock, NULL);
    else
        sceKernelLockMutex(lock, 1, NULL);
    l_locked = 1;
    for (volatile int n = 0; n < SPINS && !l_go; n++)
        ;
    record(1);
    if (is_rwlock)
        sceKernelUnlockWriteRWLock(lock);
    else
        sceKernelUnlockMutex(lock, 1);
    return 0;
}

static int m_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(2);
    return 0;
}

static void schedule_case(const char *name, int rwlock) {
    is_rwlock = rwlock;
    l_locked = l_go = 0;
    step_count = 0;
    for (int i = 0; i < MAX_STEPS; i++)
        steps[i] = -1;
    lock = rwlock ? sceKernelCreateRWLock("rw", 0x4, NULL) : create_ceiling(CEILING);
    const SceUID l = sceKernelCreateThread("l", l_entry, OWNER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(l, 0, NULL);
    TEST_WAIT_FOR(l_locked);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(l, &info);
    const SceUID m = sceKernelCreateThread("m", m_entry, MIDDLE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(m, 0, NULL);
    l_go = 1;
    test_join_thread(l);
    test_join_thread(m);
    if (rwlock)
        sceKernelDeleteRWLock(lock);
    else
        sceKernelDeleteMutex(lock);
    PRINTF(info.currentPriority, "%s: the owner's current priority in its info", name);
    PRINTF(info.initPriority, "%s: the owner's initial priority in its info", name);
    const int count = step_count < MAX_STEPS ? step_count : MAX_STEPS;
    for (int i = 0; i < count; i++)
        PRINTF(steps[i], "%s: step %d", name, i);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);

    creates();
    owner_case("an owner below the ceiling", CEILING, 0, OWNER_PRIORITY);
    owner_case("an owner above the ceiling", CEILING, 0, 0x70);
    owner_case("nested ceilings 0x90 then 0x80", 0x90, 0x80, OWNER_PRIORITY);
    owner_case("nested ceilings 0x80 then 0x90", 0x80, 0x90, OWNER_PRIORITY);
    owner_case("a ceiling of 0x70 for an owner at 0xB0", 0x70, 0, OWNER_PRIORITY);
    change_case();
    schedule_case("a mutex with a ceiling of 0x80, a middle priority thread on the owner's core", 0);
    schedule_case("a rwlock with the bit 0x4, a middle priority thread on the owner's core", 1);

    test_exit();
    return 0;
}
