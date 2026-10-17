// Which cores the threads that one call wakes go to, when they may run on several cores and each
// core runs a thread of another priority. The waiters first run on core 0, where they wait on one
// semaphore in the order W0, W1, W2, then their mask becomes the cores of the case. Spinner
// threads loop without syscalls on cores 0 and 1. One signal of the semaphore wakes every waiter,
// and each records the core it runs on, then spins, so the core it took stays taken. The cases:
// - two other cores: W0 and W1 may use cores 0 and 1, which run spinners at 0xB0 and 0xB8, and the
//   main thread signals from core 2 at 0x48. It waits up to 200 ms for both to run, notes their
//   statuses and current CPUs, then lets them end. A status of STANDBY is printed as READY, since
//   which of the two a thread queued behind another on a busy core shows varies between runs.
// - three cores, the waker's the lowest: W0, W1 and W2 may use every core, cores 0 and 1 run
//   spinners at 0xB0 and 0xB8, and the main thread signals from core 2 at 0xBC. A waiter ends
//   once the others ran, or 300 ms after it started to run.
// Each case runs with the waiters at 0x70, in the queues of one core, and at 0xA0, in the queue
// the cores share, and prints the core each waiter ran on. Waiters on different cores run at the
// same time, so the order they ran in isn't printed. In the first case at 0x70 only W0 is
// printed, since the core W1 takes there varies between runs.
//
// On a PS Vita TV on firmware 3.65 at 0x70 W0 runs on core 1, which runs the lowest priority of its
// mask, and W1 went behind it on core 1 in five of six runs and to core 0 in the other. With the
// waker on core 2 running the lowest priority, all three waiters go to core 2 and run there one
// after another while cores 0 and 1 keep their spinners, so one call piles the waiters of the
// queues of one core onto one core. At 0xA0, in the queue the cores share, they spread: W0 on core
// 1 and W1 on core 0, and W0, W1 and W2 on cores 2, 1 and 0.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAX_WAITERS 3
#define SPINS 400000000
#define TWO_CORES (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1)

static SceUID sema;
static volatile int spinner_up[2], spinner_stop[2], release, self_release, count, ran;
static volatile int cpus[MAX_WAITERS];

static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    const int core = *(const int *)argp;
    spinner_up[core] = 1;
    for (volatile int n = 0; n < SPINS && !spinner_stop[core]; n++)
        ;
    return 0;
}

static SceUID spinners[2];

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
    sceKernelWaitThreadEnd(spinners[core], NULL, NULL);
    sceKernelDeleteThread(spinners[core]);
}

// Waits on the semaphore, records its core, then keeps it until released
static int waiter_entry(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 timeout = 2 * TEST_WAIT_US;
    if (sceKernelWaitSema(sema, 1, &timeout) < 0)
        return 0;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    cpus[i] = info.currentCpuId;
    __atomic_add_fetch(&ran, 1, __ATOMIC_SEQ_CST);
    if (self_release) {
        const SceUInt64 start = sceKernelGetProcessTimeWide();
        while (ran < count && sceKernelGetProcessTimeWide() - start < 300000)
            ;
    } else {
        for (volatile int n = 0; n < SPINS && !release; n++)
            ;
    }
    return 0;
}

static void run_case(const char *name, int priority, int three_cores) {
    count = three_cores ? 3 : 2;
    self_release = three_cores;
    release = ran = 0;
    for (int i = 0; i < MAX_WAITERS; i++)
        cpus[i] = -1;
    sceKernelChangeThreadPriority(0, three_cores ? 0xBC : 0x48);
    sema = sceKernelCreateSema("sema", 0, 0, MAX_WAITERS, NULL);
    const SceUInt32 mask = three_cores ? SCE_KERNEL_CPU_MASK_USER_ALL : TWO_CORES;
    SceUID waiters[MAX_WAITERS];
    for (int i = 0; i < count; i++) {
        waiters[i] = sceKernelCreateThread("waiter", waiter_entry, priority, 0x4000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_0, NULL);
        sceKernelStartThread(waiters[i], sizeof(i), &i);
        TEST_WAIT_FOR(test_in_wait(waiters[i], WAIT_TYPE_SEMA, sema));
        sceKernelChangeThreadCpuAffinityMask(waiters[i], mask);
    }
    spinner_start(0, 0xB0);
    spinner_start(1, 0xB8);

    sceKernelSignalSema(sema, count);
    SceKernelThreadInfo infos[MAX_WAITERS] = { 0 };
    if (!three_cores) {
        for (int i = 0; i < 200 && ran < count; i++)
            sceKernelDelayThread(1000);
        for (int i = 0; i < count; i++) {
            infos[i].size = sizeof(infos[i]);
            sceKernelGetThreadInfo(waiters[i], &infos[i]);
        }
        release = 1;
    }
    TEST_WAIT_FOR(ran == count);
    release = 1;
    for (int i = 0; i < count; i++)
        test_join_thread(waiters[i]);
    spinner_end(0);
    spinner_end(1);
    sceKernelDeleteSema(sema);
    sceKernelChangeThreadPriority(0, 0x48);

    // At 0x70 with two cores, W1 goes behind W0 or to the other core, depending on when the core
    // W0 went to switches to it
    const int printed = !three_cores && priority < 0x80 ? 1 : count;
    if (!three_cores) {
        for (int i = 0; i < printed; i++) {
            const int status
                = infos[i].status == SCE_THREAD_STANDBY ? SCE_THREAD_READY : infos[i].status;
            PRINTF(status, "%s, at 0x%X: W%d's status after the signal", name, priority, i);
            PRINTF(infos[i].currentCpuId, "%s, at 0x%X: W%d's current CPU after the signal", name,
                   priority, i);
        }
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
        run_case("two other cores", priorities[p], 0);
        run_case("three cores, the waker's the lowest", priorities[p], 1);
    }
    test_exit();
    return 0;
}
