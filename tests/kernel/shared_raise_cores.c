// Which core's queue a priority change puts a thread in when it moves the thread from the queue
// the cores share into the queues of one core, depending on the core the change comes from and
// on the thread's mask. shared_ready_raise has the change from core 1 of a thread on core 0
// alone. Spinner threads at 0x40 hold the cores of T's mask without syscalls, and the main thread
// runs on core 1 at 0x50. T, at 0xA0, is started from core 1, so it waits READY in the queue the
// cores share (ready_cpu_field), then is raised to 0x60, a priority of the queues of one core.
// 10 ms after the change, while the thread that made it still holds its core, that thread or the
// main thread notes T's status and current CPU. The main thread then prints those, whether T ran
// within 200 ms, while the spinners still held their cores, the core T ran on and the priority it
// ran at. The cases:
// - T on core 0 alone, raised by a helper thread on core 2 at 0x50, which then ends
// - T on core 0 alone, raised by the spinner of core 0 itself
// - T on cores 0 and 2, both held by spinners, raised by the main thread
// - T on core 0 alone, with the VM attribute, suspended by the main thread, raised while
//   suspended, then resumed
// - T at 0x70 on core 0 alone, made READY in the queue of core 0 by a signal from the spinner
//   (wake_core_origin), changed by the main thread to 0x68, another priority of those queues
// - the same T lowered by the main thread to 0xA0 and raised back to 0x70
// A T that no core takes once the spinners end gets every core and its priority back.
//
// On a PS Vita TV on firmware 3.65 a raise of T from the queue the cores share to 0x60 puts T in
// the queue of the core the change runs on, inside its mask or not: raised from core 2, or from
// core 1 with the mask of cores 0 and 2, T reports READY on that core and runs there at 0x60 while
// the spinners hold their cores. Raised by the spinner of core 0, T waits READY on core 0 and runs
// there once the spinner ends. A raise while T is suspended for VM doesn't: the resume from core 1
// makes it STANDBY on core 0, and it runs there. A change of a READY T at 0x70 to 0x68 from core 1
// leaves it on core 0, while a change to 0xA0 and back to 0x70 from core 1 takes it to core 1,
// where it runs at 0x70.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define SPINNER_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define MAIN_PRIORITY 0x50
#define RAISED 0x60
#define SPINS 400000000

enum Case { FROM_CORE_2, FROM_CORE_0, TWO_CORES, SUSPENDED, LOCAL_CHANGE, LOCAL_ROUND_TRIP };
enum Action { NONE, SIGNAL, RAISE };

static SceUID sema, t, spinners[3];
static volatile int spinner_up[3], spinner_stop[3], action, acted, spinner_result;
static volatile int t_ran, t_cpu, t_priority, helper_result;
static SceKernelThreadInfo helper_info;

// Waits about us microseconds without blocking, so the caller keeps its core
static void busy_wait(SceUInt64 us) {
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    while (sceKernelGetProcessTimeWide() - start < us)
        ;
}

// Holds its core, and the spinner of core 0 also signals T or raises it when told to
static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    const int core = *(const int *)argp;
    spinner_up[core] = 1;
    for (volatile int n = 0; n < SPINS && !spinner_stop[core]; n++) {
        if (core == 0 && action != NONE && acted != action) {
            if (action == SIGNAL)
                sceKernelSignalSema(sema, 1);
            else
                spinner_result = sceKernelChangeThreadPriority(t, RAISED);
            acted = action;
        }
    }
    return 0;
}

static void spinner_start(int core) {
    spinner_up[core] = spinner_stop[core] = 0;
    const SceUInt32 mask = core ? SCE_KERNEL_CPU_MASK_USER_2 : SCE_KERNEL_CPU_MASK_USER_0;
    spinners[core] = sceKernelCreateThread("spinner", spinner_entry, SPINNER_PRIORITY, 0x4000, 0,
                                           mask, NULL);
    sceKernelStartThread(spinners[core], sizeof(core), &core);
    TEST_WAIT_FOR(spinner_up[core]);
}

static void spinner_end(int core) {
    spinner_stop[core] = 1;
    test_join_thread(spinners[core]);
}

// Has the spinner of core 0 do what and waits until it did
static void spinner_do(int what) {
    action = what;
    TEST_WAIT_FOR(acted == what);
}

// T, which waits for the semaphore first when started with an argument
static int t_entry(SceSize args, void *argp) {
    (void)argp;
    if (args) {
        SceUInt32 timeout = 3 * TEST_WAIT_US;
        sceKernelWaitSema(sema, 1, &timeout);
    }
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    t_cpu = info.currentCpuId;
    t_priority = sceKernelGetThreadCurrentPriority();
    t_ran = 1;
    return 0;
}

// The helper on core 2, which raises T, then notes T's info 10 ms later, before it ends
static int helper_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    helper_result = sceKernelChangeThreadPriority(t, RAISED);
    busy_wait(10000);
    sceKernelGetThreadInfo(t, &helper_info);
    return 0;
}

static void run_case(const char *name, int c) {
    t_ran = 0;
    t_cpu = t_priority = -1;
    action = acted = NONE;
    spinner_result = helper_result = -1;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const int local = c == LOCAL_CHANGE || c == LOCAL_ROUND_TRIP;
    const int t_initial = local ? 0x70 : 0xA0;
    SceUInt32 mask = SCE_KERNEL_CPU_MASK_USER_0;
    if (c == TWO_CORES)
        mask |= SCE_KERNEL_CPU_MASK_USER_2;
    t = sceKernelCreateThread("t", t_entry, t_initial, 0x4000, c == SUSPENDED ? THREAD_ATTR_VM : 0,
                              mask, NULL);
    if (local) {
        int wait = 1;
        sceKernelStartThread(t, sizeof(wait), &wait);
        TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, sema));
    }
    spinner_start(0);
    if (c == TWO_CORES)
        spinner_start(2);
    if (local)
        spinner_do(SIGNAL);
    else
        sceKernelStartThread(t, 0, NULL);

    int result = 0, suspend = 0, resume = 0;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (c == FROM_CORE_2) {
        helper_info.size = sizeof(helper_info);
        const SceUID helper = sceKernelCreateThread("helper", helper_entry, MAIN_PRIORITY, 0x4000,
                                                    0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
        sceKernelStartThread(helper, 0, NULL);
        test_join_thread(helper);
        result = helper_result;
        info = helper_info;
    } else {
        if (c == FROM_CORE_0) {
            spinner_do(RAISE);
            result = spinner_result;
        } else if (c == TWO_CORES) {
            result = sceKernelChangeThreadPriority(t, RAISED);
        } else if (c == SUSPENDED) {
            suspend = sceKernelSuspendThreadForVM(t);
            result = sceKernelChangeThreadPriority(t, RAISED);
            resume = sceKernelResumeThreadForVM(t);
        } else if (c == LOCAL_CHANGE) {
            result = sceKernelChangeThreadPriority(t, 0x68);
        } else {
            result = sceKernelChangeThreadPriority(t, 0xA0);
            result |= sceKernelChangeThreadPriority(t, 0x70);
        }
        busy_wait(10000);
        sceKernelGetThreadInfo(t, &info);
    }
    for (int i = 0; i < 200 && !t_ran; i++)
        sceKernelDelayThread(1000);
    const int ran_early = t_ran;

    spinner_end(0);
    if (c == TWO_CORES)
        spinner_end(2);
    TEST_WAIT_FOR(t_ran);
    // A T left in a queue that no core takes it from gets a mask and priority any core takes
    const int rescued = !t_ran;
    if (rescued) {
        sceKernelChangeThreadCpuAffinityMask(t, SCE_KERNEL_CPU_MASK_USER_ALL);
        sceKernelChangeThreadPriority(t, t_initial);
        TEST_WAIT_FOR(t_ran);
    }
    SceUInt timeout = TEST_WAIT_US;
    if (sceKernelWaitThreadEnd(t, NULL, &timeout) >= 0)
        sceKernelDeleteThread(t);
    sceKernelDeleteSema(sema);

    if (c == SUSPENDED)
        PRINTF(suspend, "%s: the suspend", name);
    PRINTF(result, "%s: the change", name);
    if (c == SUSPENDED)
        PRINTF(resume, "%s: the resume", name);
    PRINTF(info.status, "%s: T's status 10 ms after the change", name);
    PRINTF(info.currentCpuId, "%s: T's current CPU 10 ms after the change", name);
    PRINTF(ran_early, "%s: T ran while the spinners held their cores", name);
    PRINTF(rescued, "%s: T only ran once given every core and its priority back", name);
    PRINTF(t_cpu, "%s: the core T ran on", name);
    PRINTF(t_priority, "%s: the priority T ran at", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case("raised from core 2", FROM_CORE_2);
    run_case("raised by the spinner of core 0", FROM_CORE_0);
    run_case("on cores 0 and 2, raised from core 1", TWO_CORES);
    run_case("suspended, raised and resumed from core 1", SUSPENDED);
    run_case("READY at 0x70 on core 0, changed to 0x68 from core 1", LOCAL_CHANGE);
    run_case("READY at 0x70 on core 0, changed to 0xA0 and back from core 1", LOCAL_ROUND_TRIP);
    test_exit();
    return 0;
}
