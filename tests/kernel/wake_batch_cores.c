// Where the threads one call wakes go, next to threads woken one call after another, when they may
// run on cores 0 and 1 and both cores run a thread of a lower priority. The waiters, W0, W1 and
// W2, first run on core 0, where they wait in that order, then their mask becomes cores 0 and 1.
// Spinner threads loop without syscalls on core 0 at 0xB0 and on core 1 at 0xB8, and the main
// thread wakes the waiters from core 2 at 0x48. Each waiter records the core it runs on, then
// spins until the main thread lets it end, so the core it took stays taken. The main thread waits
// up to 200 ms for all of them to run, notes their statuses and current CPUs, then lets them end
// one at a time, in their order. The wakes:
// - a lone waiter, woken by a signal of 1
// - one signal of 3
// - three signals of 1, one call after another
// - one set of an event flag that all three wait on
// Each runs with the waiters at 0x70, in the queues of one core, and at 0xA0, in the queue the
// cores share, and prints the core each waiter ran on. A status of STANDBY is printed as READY,
// since which of the two a thread queued behind another on a busy core shows varies between runs.
// For one signal of 3 and the event flag at 0x70 only W0 is printed, since where W1 and W2 go
// varies between runs.
// wake_spread has one signal of 2.
//
// On a PS Vita TV on firmware 3.65 a lone waiter goes to core 1, which runs the lowest priority.
// Three signals of 1 at 0x70 send W0 to core 1 and W1 to core 0, and W2 waits behind W1 on core 0.
// One signal of 3 and one event flag set at 0x70 send W0 to core 1, and where W1 and W2 went varied
// between runs. At 0xA0 every kind of wake places them alike: W0 runs on core 1, W1 on core 0, and
// W2 waits READY in the queue the cores share, then takes core 1 after W0.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAX_WAITERS 3
#define SPINS 400000000
#define TWO_CORES (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1)

enum Wake { LONE, ONE_CALL, THREE_CALLS, EVENT_FLAG };

static SceUID sema, flag, spinners[2];
static volatile int wake, spinner_up[2], spinner_stop[2], released, ran;
static volatile int cpus[MAX_WAITERS];

static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    const int core = *(const int *)argp;
    spinner_up[core] = 1;
    for (volatile int n = 0; n < SPINS && !spinner_stop[core]; n++)
        ;
    return 0;
}

static void spinner_start(int core, int priority) {
    spinner_up[core] = spinner_stop[core] = 0;
    const SceUInt32 mask = core ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_0;
    spinners[core] = sceKernelCreateThread("spinner", spinner_entry, priority, 0x4000, 0, mask,
                                           NULL);
    sceKernelStartThread(spinners[core], sizeof(core), &core);
    TEST_WAIT_FOR(spinner_up[core]);
}

static void spinner_end(int core) {
    spinner_stop[core] = 1;
    test_join_thread(spinners[core]);
}

// Waits to be woken, records its core, then keeps it until the main thread released waiters up to
// it
static int waiter_entry(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 timeout = 2 * TEST_WAIT_US;
    int result;
    unsigned int bits;
    if (wake == EVENT_FLAG)
        result = sceKernelWaitEventFlag(flag, 1, SCE_EVENT_WAITOR, &bits, &timeout);
    else
        result = sceKernelWaitSema(sema, 1, &timeout);
    if (result < 0)
        return 0;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    cpus[i] = info.currentCpuId;
    __atomic_add_fetch(&ran, 1, __ATOMIC_SEQ_CST);
    for (volatile int n = 0; n < SPINS && released <= i; n++)
        ;
    return 0;
}

static void run_case(const char *name, int how, int priority) {
    wake = how;
    const int count = how == LONE ? 1 : MAX_WAITERS;
    released = ran = 0;
    for (int i = 0; i < MAX_WAITERS; i++)
        cpus[i] = -1;
    sema = sceKernelCreateSema("sema", 0, 0, MAX_WAITERS, NULL);
    flag = sceKernelCreateEventFlag("flag", SCE_EVENT_WAITMULTIPLE, 0, NULL);
    SceUID waiters[MAX_WAITERS];
    for (int i = 0; i < count; i++) {
        waiters[i] = sceKernelCreateThread("waiter", waiter_entry, priority, 0x4000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_0, NULL);
        sceKernelStartThread(waiters[i], sizeof(i), &i);
        if (how == EVENT_FLAG)
            TEST_WAIT_FOR(test_in_wait(waiters[i], WAIT_TYPE_EVENT_FLAG, flag));
        else
            TEST_WAIT_FOR(test_in_wait(waiters[i], WAIT_TYPE_SEMA, sema));
        sceKernelChangeThreadCpuAffinityMask(waiters[i], TWO_CORES);
    }
    spinner_start(0, 0xB0);
    spinner_start(1, 0xB8);

    if (how == LONE) {
        sceKernelSignalSema(sema, 1);
    } else if (how == ONE_CALL) {
        sceKernelSignalSema(sema, MAX_WAITERS);
    } else if (how == THREE_CALLS) {
        for (int i = 0; i < MAX_WAITERS; i++)
            sceKernelSignalSema(sema, 1);
    } else {
        sceKernelSetEventFlag(flag, 1);
    }
    for (int i = 0; i < 200 && ran < count; i++)
        sceKernelDelayThread(1000);
    SceKernelThreadInfo infos[MAX_WAITERS];
    for (int i = 0; i < count; i++) {
        infos[i].size = sizeof(infos[i]);
        sceKernelGetThreadInfo(waiters[i], &infos[i]);
    }
    // One at a time, so a waiter that waits for a core takes the core of the one that ended
    for (int i = 0; i < count; i++) {
        released = i + 1;
        SceUInt timeout = TEST_WAIT_US;
        if (sceKernelWaitThreadEnd(waiters[i], NULL, &timeout) < 0)
            released = count;
    }
    for (int i = 0; i < count; i++)
        test_join_thread(waiters[i]);
    spinner_end(0);
    spinner_end(1);
    sceKernelDeleteSema(sema);
    sceKernelDeleteEventFlag(flag);

    const int printed = (how == ONE_CALL || how == EVENT_FLAG) && priority < 0x80 ? 1 : count;
    for (int i = 0; i < printed; i++) {
        const int status
            = infos[i].status == SCE_THREAD_STANDBY ? SCE_THREAD_READY : infos[i].status;
        PRINTF(status, "%s, at 0x%X: W%d's status after the wake", name, priority, i);
        PRINTF(infos[i].currentCpuId, "%s, at 0x%X: W%d's current CPU after the wake", name,
               priority, i);
    }
    for (int i = 0; i < printed; i++)
        PRINTF(cpus[i], "%s, at 0x%X: the core W%d ran on", name, priority, i);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    sceKernelChangeThreadPriority(0, 0x48);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);
    static const int priorities[2] = { 0x70, 0xA0 };
    for (int p = 0; p < 2; p++) {
        run_case("a lone waiter", LONE, priorities[p]);
        run_case("one signal of 3", ONE_CALL, priorities[p]);
        run_case("three signals of 1", THREE_CALLS, priorities[p]);
        run_case("one event flag set", EVENT_FLAG, priorities[p]);
    }
    test_exit();
    return 0;
}
