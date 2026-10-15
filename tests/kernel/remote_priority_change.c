// Whether a priority change from another core takes a core from the thread spinning there at
// once. L spins on core 0 without syscalls, and R, of a lower priority, is started from core 1
// and waits behind it. The main thread, on core 1, then either lowers L below R, or raises R
// above L, and prints whether R ran within 10 ms, and L's status then. L also runs once with a
// syscall on every loop, to see whether its next syscall gives R the core after L is lowered:
// - L at 0x70, R at 0x78, in the queue of core 0, with L lowered to 0x7C or R raised to 0x6C
// - L at 0xA0, R at 0xA8, in the queue the cores share, with L lowered to 0xAC or R raised to
//   0x9C
//
// On a PS Vita TV on firmware 3.65 raising R above L gives R the core at once at both
// priorities, and so does lowering L at 0xA0. Lowering L at 0x70 doesn't: L keeps the core,
// RUNNING, and R waits, also when L makes a syscall on every loop. A lowered priority only gives
// the core away when the core next switches threads.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define SPINS 400000000

static volatile int l_up, l_stop, r_ran;

// L, which makes a syscall on every loop when started with an argument
static int l_entry(SceSize args, void *argp) {
    (void)argp;
    l_up = 1;
    for (volatile int n = 0; n < SPINS && !l_stop; n++) {
        if (args)
            sceKernelGetThreadId();
    }
    return 0;
}

static int r_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    r_ran = 1;
    return 0;
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

static void run_case(int l_priority, int lower_l, int syscalls) {
    l_up = l_stop = r_ran = 0;
    const int r_priority = l_priority + 8;
    const SceUID l = sceKernelCreateThread("l", l_entry, l_priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    if (syscalls)
        sceKernelStartThread(l, sizeof(syscalls), &syscalls);
    else
        sceKernelStartThread(l, 0, NULL);
    TEST_WAIT_FOR(l_up);
    const SceUID r = sceKernelCreateThread("r", r_entry, r_priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(r, 0, NULL);
    sceKernelDelayThread(10000);
    const int r_ran_before = r_ran;
    if (lower_l)
        sceKernelChangeThreadPriority(l, r_priority + 4);
    else
        sceKernelChangeThreadPriority(r, l_priority - 4);
    sceKernelDelayThread(10000);
    const int r_ran_after = r_ran;
    const SceUInt32 l_status = status_of(l);
    l_stop = 1;
    test_join_thread(l);
    test_join_thread(r);
    const char *const how = lower_l ? (syscalls ? "L, making syscalls, lowered below R" : "L lowered below R") : "R raised above L";
    PRINTF(r_ran_before, "L at 0x%X, %s: R ran before the change", l_priority, how);
    PRINTF(r_ran_after, "L at 0x%X, %s: R ran within 10 ms of the change", l_priority, how);
    PRINTF(l_status, "L at 0x%X, %s: L's status then", l_priority, how);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    static const int priorities[2] = { 0x70, 0xA0 };
    for (int p = 0; p < 2; p++) {
        run_case(priorities[p], 1, 0);
        run_case(priorities[p], 1, 1);
        run_case(priorities[p], 0, 0);
    }
    test_exit();
    return 0;
}
