// What a thread preempted on its core looks like, and whether it gets the core back before a
// thread of its priority made ready behind it. L spins on core 0 without syscalls, and M, of the
// same priority, is started from core 1 behind it. Then a signal from the main thread, on core
// 1, wakes T at 0x70, waiting on core 0, which takes the core from L. T reads the statuses of L
// and M while it runs, and ends. Since L never yields, M only runs before L stops if M got the
// core first. Each case prints the statuses T saw, and whether M ran within 50 ms of T's end,
// with L and M at 0xB0, in the queue the cores share, and at 0x78, in the one of core 0.
//
// On a PS Vita TV on firmware 3.65 the preempted L is READY while T runs, and gets core 0 back
// before M at both priorities. At 0x78 M is STANDBY behind the running L, and READY once T took
// the core, so the core's schedule took it into its ready queue.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define T_PRIORITY 0x70
#define SPINS 400000000

static SceUID sema, l, m;
static volatile int l_up, l_stop, m_ran, t_done;
static volatile SceUInt32 l_status, m_status;

static int l_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    l_up = 1;
    for (volatile int n = 0; n < SPINS && !l_stop; n++)
        ;
    return 0;
}

static int m_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    m_ran = 1;
    return 0;
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    l_status = status_of(l);
    m_status = status_of(m);
    t_done = 1;
    return 0;
}

static void run_case(int priority) {
    l_up = l_stop = m_ran = t_done = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID t = sceKernelCreateThread("t", t_entry, T_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);
    test_wait_until_waiting(t);
    l = sceKernelCreateThread("l", l_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(l, 0, NULL);
    TEST_WAIT_FOR(l_up);
    m = sceKernelCreateThread("m", m_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(m, 0, NULL);
    sceKernelDelayThread(10000);
    const SceUInt32 m_before = status_of(m);

    sceKernelSignalSema(sema, 1);
    TEST_WAIT_FOR(t_done);
    test_join_thread(t);
    sceKernelDelayThread(50000);
    const int m_first = m_ran;
    l_stop = 1;
    TEST_WAIT_FOR(m_ran);
    PRINTF(m_before, "L and M at 0x%X: M's status behind the running L", priority);
    PRINTF(l_status, "L and M at 0x%X: L's status while T runs", priority);
    PRINTF(m_status, "L and M at 0x%X: M's status while T runs", priority);
    PRINTF(m_first, "L and M at 0x%X: M ran before L got the core back", priority);
    test_join_thread(l);
    test_join_thread(m);
    sceKernelDeleteSema(sema);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case(0xB0);
    run_case(0x78);
    test_exit();
    return 0;
}
