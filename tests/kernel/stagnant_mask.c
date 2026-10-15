// What a CPU mask with only the system core does to a thread, the case where no usable core is
// in its mask. T runs at 0x70. The main thread, on core 1, gives T the mask
// SCE_KERNEL_CPU_MASK_SYSTEM while T is dormant, waiting, or running in a loop that counts, then
// wakes or starts T as needed, and prints the result of the change, T's status 10 ms later, and
// whether T made progress. Then it gives T core 0 back and prints whether T makes progress
// again. It also prints what creating a thread with that mask returns.
//
// On a PS Vita TV on firmware 3.65 the mask is refused with 0x80028025 at create and in every
// change, alone or with the user cores, and T goes on as before, so a mask can't make a thread
// STAGNANT.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define SPINS 400000000

static SceUID sema;
static volatile int t_up, t_stop;
static volatile unsigned int t_count;

static int counting_entry(SceSize args, void *argp) {
    (void)argp;
    // Started with an argument, T waits for the semaphore first
    if (args) {
        SceUInt32 timeout = TEST_WAIT_US;
        sceKernelWaitSema(sema, 1, &timeout);
    }
    t_up = 1;
    for (volatile int n = 0; n < SPINS && !t_stop; n++)
        t_count++;
    return 0;
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

// Whether t_count moves within 10 ms
static int progresses(void) {
    const unsigned int before = t_count;
    sceKernelDelayThread(10000);
    return t_count != before;
}

// Waits up to 2 s for thread to end, and deletes it if it did
static int join(SceUID thread) {
    SceUInt32 timeout = TEST_WAIT_US;
    const int r = sceKernelWaitThreadEnd(thread, NULL, &timeout);
    if (r == 0)
        sceKernelDeleteThread(thread);
    return r;
}

enum Case { DORMANT, WAITING, RUNNING, CASE_COUNT };
static const char *const case_names[CASE_COUNT] = { "dormant", "waiting", "running" };

static void run_case(int c) {
    t_up = t_stop = 0;
    t_count = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID t = sceKernelCreateThread("t", counting_entry, 0x70, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    int wait = 1;
    if (c == WAITING) {
        sceKernelStartThread(t, sizeof(wait), &wait);
        test_wait_until_waiting(t);
    } else if (c == RUNNING) {
        sceKernelStartThread(t, 0, NULL);
        TEST_WAIT_FOR(t_up);
    }
    const int changed = sceKernelChangeThreadCpuAffinityMask(t, SCE_KERNEL_CPU_MASK_SYSTEM);
    if (c == DORMANT)
        PRINTF(sceKernelStartThread(t, 0, NULL), "%s: start it", case_names[c]);
    else if (c == WAITING)
        sceKernelSignalSema(sema, 1);
    TEST_WAIT_FOR(t_up);
    PRINTF(changed, "%s: the change to the system core, the old mask or an error", case_names[c]);
    PRINTF(status_of(t), "%s: its status after", case_names[c]);
    PRINTF(progresses(), "%s: it makes progress after", case_names[c]);
    PRINTF(sceKernelChangeThreadCpuAffinityMask(t, SCE_KERNEL_CPU_MASK_USER_0), "%s: the change back to core 0, the old mask or an error", case_names[c]);
    PRINTF(progresses(), "%s: it makes progress after that", case_names[c]);
    t_stop = 1;
    PRINTF(join(t), "%s: its end", case_names[c]);
    sceKernelDeleteSema(sema);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    const SceUID made = sceKernelCreateThread("system", counting_entry, 0x70, 0x4000, 0, SCE_KERNEL_CPU_MASK_SYSTEM, NULL);
    PRINT("create a thread with the system core alone", made < 0 ? made : 0);
    if (made >= 0)
        sceKernelDeleteThread(made);
    const SceUID all = sceKernelCreateThread("all", counting_entry, 0x70, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_ALL | SCE_KERNEL_CPU_MASK_SYSTEM, NULL);
    PRINT("create a thread with the user cores and the system core", all < 0 ? all : 0);
    if (all >= 0)
        sceKernelDeleteThread(all);
    for (int c = 0; c < CASE_COUNT; c++)
        run_case(c);
    test_exit();
    return 0;
}
