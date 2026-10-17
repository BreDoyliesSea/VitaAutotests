// Which core a thread that may run on every core goes to when a start or the end of its delay
// makes it ready, while each core runs a thread of another priority below its own, like
// wake_target_core does for a signal. Spinner threads loop without syscalls on cores 0, 1 and 2,
// and the main thread runs on core 2 above them all. The cases:
// - a start: T never ran. The spinners run at 0xB8 on the core of the lowest priority, 0xB0 on
//   the core of the starter and 0xB4 on the third core, and the starter's spinner starts T once
//   the main thread waits on a semaphore.
// - the end of a delay: T first runs on its last core and delays itself 200 ms there, then its
//   mask becomes every core. The spinners run at 0xB8 on the core of the lowest priority, 0xB0
//   on T's last core and 0xB4 on the third core, and the main thread waits on a semaphore
//   before the delay ends.
// So every core runs its spinner when T becomes ready. T records the core it runs on. Each order
// of the cores runs with T at 0x70, in the queues of one core, and the starts also at 0xA0, in the
// queue the cores share. Which core takes a thread at 0xA0 whose delay ended varies between runs,
// so those cases aren't run.
//
// On a PS Vita TV on firmware 3.65 a started T goes to the core that runs the lowest priority in
// every order, whichever core starts it, at 0x70 and at 0xA0. At 0x70 a T whose delay ended goes
// there too, wherever it last ran. At 0xA0 a T whose delay ended went to another core in one of
// three runs, so those cases aren't run.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x48
#define SPINS 400000000

static SceUID done, main_thread, t, spinners[3];
static volatile int spinner_up[3], spinner_stop[3], starter, starter_go, started, t_cpu;

static SceUInt32 mask_of(int core) {
    return SCE_KERNEL_CPU_MASK_USER_0 << core;
}

// Whether thread is in a wait of the given type
static int waits_with(SceUID thread, SceUInt32 type) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && info.waitType == type;
}

// Holds its core, and the spinner of the starter's core starts T once the main thread waits
static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    const int core = *(const int *)argp;
    spinner_up[core] = 1;
    for (volatile int n = 0; n < SPINS && !spinner_stop[core]; n++) {
        if (core == starter && starter_go && !started && waits_with(main_thread, WAIT_TYPE_SEMA)) {
            sceKernelStartThread(t, 0, NULL);
            started = 1;
        }
    }
    return 0;
}

static void spinner_start(int core, int priority) {
    spinner_up[core] = spinner_stop[core] = 0;
    spinners[core] = sceKernelCreateThread("spinner", spinner_entry, priority, 0x4000, 0,
                                           mask_of(core), NULL);
    sceKernelStartThread(spinners[core], sizeof(core), &core);
    TEST_WAIT_FOR(spinner_up[core]);
}

// T, which delays itself 200 ms first when started with an argument
static int t_entry(SceSize args, void *argp) {
    (void)argp;
    if (args)
        sceKernelDelayThread(200000);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    t_cpu = info.currentCpuId;
    sceKernelSignalSema(done, 1);
    return 0;
}

static void run_case(int delay, int lowest, int other, int t_priority) {
    t_cpu = -1;
    starter = delay ? -1 : other;
    starter_go = started = 0;
    done = sceKernelCreateSema("done", 0, 0, 1, NULL);
    t = sceKernelCreateThread("t", t_entry, t_priority, 0x4000, 0,
                              delay ? mask_of(other) : SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    if (delay) {
        int arg = 1;
        sceKernelStartThread(t, sizeof(arg), &arg);
        TEST_WAIT_FOR(waits_with(t, WAIT_TYPE_DELAY));
        sceKernelChangeThreadCpuAffinityMask(t, SCE_KERNEL_CPU_MASK_USER_ALL);
    }
    for (int core = 0; core < 3; core++)
        spinner_start(core, core == lowest ? 0xB8 : core == other ? 0xB0 : 0xB4);
    starter_go = 1;
    SceUInt32 timeout = 2 * TEST_WAIT_US;
    sceKernelWaitSema(done, 1, &timeout);
    for (int core = 0; core < 3; core++) {
        spinner_stop[core] = 1;
        test_join_thread(spinners[core]);
    }
    test_join_thread(t);
    sceKernelDeleteSema(done);
    const char *const role = delay ? "last" : "the starter";
    PRINTF(t_cpu, "a %s, the lowest on core %d, %s on core %d, T at 0x%X: the core T ran on",
           delay ? "delay" : "start", lowest, role, other, t_priority);
}

int main(void) {
    main_thread = sceKernelGetThreadId();
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);
    static const int t_priorities[2] = { 0x70, 0xA0 };
    for (int delay = 0; delay < 2; delay++)
        for (int p = 0; p < 2 - delay; p++)
            for (int lowest = 0; lowest < 3; lowest++)
                for (int other = 0; other < 3; other++)
                    if (other != lowest)
                        run_case(delay, lowest, other, t_priorities[p]);
    test_exit();
    return 0;
}
