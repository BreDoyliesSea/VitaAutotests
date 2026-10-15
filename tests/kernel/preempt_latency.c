// How soon a thread made ready takes its core from a lower priority thread that spins there
// without syscalls. L spins at 0xB0 on core 0, and T waits on core 0. The main thread, on core
// 1, either signals T and notes the process time just before, or lets T's own timeout end its
// wait. T notes the process time when it runs. Each case prints whether T ran within 1 ms of the
// signal or of its deadline:
// - a signal from core 1 to T at 0x70
// - T's own timeout of 100 ms at 0x70
// - a signal from core 1 to T at 0xB0, L's priority, where T only runs once L stops, so the case
//   prints whether T ran within 50 ms instead
//
// On a PS Vita TV on firmware 3.65 the signal from core 1 got T running about 40 microseconds
// later and the timeout about 50 microseconds after its deadline, on three runs, while T at L's
// priority waited until L stopped.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define L_PRIORITY 0xB0
#define T_TIMEOUT_US 100000
#define SPINS 400000000

static SceUID sema;
static volatile int l_up, l_stop, t_ran, t_waits;
static volatile SceUInt64 t_time, t_deadline;

static int l_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    l_up = 1;
    for (volatile int n = 0; n < SPINS && !l_stop; n++)
        ;
    return 0;
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    const int timed = *(const int *)argp;
    SceUInt32 timeout = timed ? T_TIMEOUT_US : TEST_WAIT_US;
    t_deadline = sceKernelGetProcessTimeWide() + T_TIMEOUT_US;
    t_waits = 1;
    sceKernelWaitSema(sema, 1, &timeout);
    t_time = sceKernelGetProcessTimeWide();
    t_ran = 1;
    return 0;
}

enum Case { SIGNAL, TIMEOUT, SAME_PRIORITY, CASE_COUNT };
static const char *const case_names[CASE_COUNT] = {
    "a signal from core 1, T at 0x70",
    "T's own timeout, T at 0x70",
    "a signal from core 1, T at L's priority",
};

static void run_case(int c) {
    l_up = l_stop = t_ran = t_waits = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    int timed = c == TIMEOUT;
    const SceUID t = sceKernelCreateThread("t", t_entry, c == SAME_PRIORITY ? L_PRIORITY : 0x70, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, sizeof(timed), &timed);
    TEST_WAIT_FOR(t_waits);
    test_wait_until_waiting(t);
    const SceUID l = sceKernelCreateThread("l", l_entry, L_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(l, 0, NULL);
    TEST_WAIT_FOR(l_up);

    SceUInt64 from;
    if (c == TIMEOUT) {
        from = t_deadline;
        sceKernelDelayThread(2 * T_TIMEOUT_US);
    } else {
        from = sceKernelGetProcessTimeWide();
        sceKernelSignalSema(sema, 1);
        sceKernelDelayThread(50000);
    }
    const int ran = t_ran;
    l_stop = 1;
    test_join_thread(t);
    test_join_thread(l);
    if (c == SAME_PRIORITY)
        PRINTF(ran, "%s: T ran within 50 ms, while L spun", case_names[c]);
    else
        PRINTF(ran && t_time - from < 1000, "%s: T ran within 1 ms", case_names[c]);
    sceKernelDeleteSema(sema);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    for (int c = 0; c < CASE_COUNT; c++)
        run_case(c);
    test_exit();
    return 0;
}
