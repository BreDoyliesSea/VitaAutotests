// Which core a woken thread that may run on cores 0 and 1 takes when the thread that wakes it runs
// on one of them. core_choice wakes from core 2, outside the mask. T first waits on one core, then
// its mask becomes cores 0 and 1. The main thread, pinned to core 0, wakes T at the priority of the
// case, while core 1 is idle or runs a spinner that loops without syscalls. The cases:
// - core 1 idle, T last ran on core 1, then on core 0, with the waker at 0xA0
// - core 1 running 0xB0, below the waker at 0xA0, T last ran on core 0
// - core 1 running 0xA0, above the waker at 0xB0, T last ran on core 1
// - core 1 running 0xA0, the waker's priority, T last ran on core 0, then on core 1
// - core 1 running 0x50, above T, T last ran on core 1, with the waker at 0xA0
// Each case runs with T at 0x70, in the queues of one core, and at 0x90, in the queue the cores
// share, woken by a semaphore signal. The case where core 1 is idle and T last ran on core 0, and
// the one where core 1 runs above the waker, also run with a mutex unlock, a lightweight mutex
// unlock, an event flag set and a simple event set. Each case prints the core T ran on, and when
// that is core 0, the waker's, whether T ran before the waking call returned.
//
// On a PS Vita TV on firmware 3.65 T goes to the core that runs the lower priority of the two,
// counting the waker's own, for every kind of wake here, at 0x70 and at 0x90. With core 1 idle or
// below the waker, T runs on core 1 wherever it last ran. With core 1 above the waker, or above T,
// T runs on core 0 and preempts the waker before the waking call returns. With core 1 at the
// waker's priority, T goes to the core it last ran on.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define SPINS 400000000

typedef enum Kind { SEMA, MUTEX, LW_MUTEX, FLAG, EVENT } Kind;

static Kind kind;
static SceUID object;
static SceKernelLwMutexWork lw_mutex;
static volatile int spinner_up, spinner_stop, t_cpu, t_ran;

static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    spinner_up = 1;
    for (volatile int n = 0; n < SPINS && !spinner_stop; n++)
        ;
    return 0;
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    SceUInt32 bits;
    switch (kind) {
    case SEMA: sceKernelWaitSema(object, 1, &timeout); break;
    case MUTEX:
        sceKernelLockMutex(object, 1, &timeout);
        sceKernelUnlockMutex(object, 1);
        break;
    case LW_MUTEX:
        sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    case FLAG: sceKernelWaitEventFlag(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout); break;
    case EVENT: sceKernelWaitEvent(object, EVENT_BIT_A, &bits, NULL, &timeout); break;
    }
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    t_cpu = info.currentCpuId;
    t_ran = 1;
    return 0;
}

static void create_object(Kind k) {
    switch (k) {
    case SEMA: object = sceKernelCreateSema("sema", 0, 0, 1, NULL); break;
    case MUTEX: object = sceKernelCreateMutex("mutex", 0, 1, NULL); break;
    case LW_MUTEX: sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 1, NULL); break;
    case FLAG: object = sceKernelCreateEventFlag("flag", 0, 0, NULL); break;
    case EVENT: object = sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET, 0, NULL); break;
    }
}

static void delete_object(Kind k) {
    switch (k) {
    case SEMA: sceKernelDeleteSema(object); break;
    case MUTEX: sceKernelDeleteMutex(object); break;
    case LW_MUTEX: sceKernelDeleteLwMutex(&lw_mutex); break;
    case FLAG: sceKernelDeleteEventFlag(object); break;
    case EVENT: sceKernelDeleteSimpleEvent(object); break;
    }
}

static void wake(Kind k) {
    switch (k) {
    case SEMA: sceKernelSignalSema(object, 1); break;
    case MUTEX: sceKernelUnlockMutex(object, 1); break;
    case LW_MUTEX: sceKernelUnlockLwMutex(&lw_mutex, 1); break;
    case FLAG: sceKernelSetEventFlag(object, 0x1); break;
    case EVENT: sceKernelSetEvent(object, EVENT_BIT_A, 0); break;
    }
}

struct Case {
    const char *name;
    int last_core, waker_priority, spinner_priority;
};

static const struct Case cases[] = {
    { "core 1 idle, last on core 1", 1, 0xA0, 0 },
    { "core 1 idle, last on core 0", 0, 0xA0, 0 },
    { "core 1 below the waker, last on core 0", 0, 0xA0, 0xB0 },
    { "core 1 above the waker, last on core 1", 1, 0xB0, 0xA0 },
    { "core 1 at the waker's priority, last on core 0", 0, 0xA0, 0xA0 },
    { "core 1 at the waker's priority, last on core 1", 1, 0xA0, 0xA0 },
    { "core 1 above T, last on core 1", 1, 0xA0, 0x50 },
};

static const char *const kind_names[] = { "semaphore signal", "mutex unlock",
    "lightweight mutex unlock", "event flag set", "simple event set" };

static void run_case(const struct Case *c, int priority, Kind k) {
    kind = k;
    t_ran = 0;
    t_cpu = -2;
    create_object(k);
    sceKernelChangeThreadPriority(0, c->waker_priority);
    const SceUID t = sceKernelCreateThread("t", t_entry, priority, 0x4000, 0,
        c->last_core ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);
    test_wait_until_waiting(t);
    sceKernelChangeThreadCpuAffinityMask(
        t, SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1);
    SceUID spinner = -1;
    if (c->spinner_priority) {
        spinner_up = spinner_stop = 0;
        spinner = sceKernelCreateThread("spinner", spinner_entry, c->spinner_priority, 0x4000, 0,
            SCE_KERNEL_CPU_MASK_USER_1, NULL);
        sceKernelStartThread(spinner, 0, NULL);
        TEST_WAIT_FOR(spinner_up);
    }
    wake(k);
    const int ran_before = t_ran;
    TEST_WAIT_FOR(t_ran);
    // Joined first, so every record T made is complete
    test_join_thread(t);
    if (spinner >= 0) {
        spinner_stop = 1;
        test_join_thread(spinner);
    }
    PRINTF(t_cpu, "%s, %s, T at 0x%X: the core T ran on", kind_names[k], c->name, priority);
    if (t_cpu == 0)
        PRINTF(ran_before, "%s, %s, T at 0x%X: T ran before the call returned", kind_names[k],
            c->name, priority);
    delete_object(k);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);
    static const int priorities[2] = { 0x70, 0x90 };
    for (int p = 0; p < 2; p++)
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
            run_case(&cases[i], priorities[p], SEMA);
    for (Kind k = MUTEX; k <= EVENT; k++)
        for (int p = 0; p < 2; p++) {
            run_case(&cases[1], priorities[p], k);
            run_case(&cases[3], priorities[p], k);
        }
    test_exit();
    return 0;
}
