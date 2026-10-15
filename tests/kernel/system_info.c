// What sceKernelGetSystemInfo reports. It prints the result for the right size, a size of 0 and
// sizes 4 bytes short and 4 bytes over, and the active CPU mask. For each user core it then has
// a thread there block and wake 10 times, and prints whether that core's thread switch count
// grew by at least 10, and whether its idle clock grew while the main thread slept 20 ms.
//
// On a PS Vita TV on firmware 3.65 the active CPU mask is 0x000F0000, the four cores, a size of
// 0 or 4 bytes short is taken, 4 bytes over fails with 0x8002710B, and every user core's
// counters grow.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static int sleeper_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (int i = 0; i < 10; i++)
        sceKernelDelayThread(1000);
    return 0;
}

// Room behind the structure for the call that is told it's 4 bytes bigger
static union {
    SceKernelSystemInfo info;
    char room[sizeof(SceKernelSystemInfo) + 4];
} padded;

static int get(SceKernelSystemInfo *info, SceSize size) {
    sceClibMemset(info, 0, sizeof(*info));
    info->size = size;
    return sceKernelGetSystemInfo(info);
}

int main(void) {
    SceKernelSystemInfo info;
    PRINT("the right size", get(&info, sizeof(info)));
    PRINT("the active CPU mask", info.activeCpuMask);
    PRINT("a size of 0", get(&info, 0));
    PRINT("4 bytes short", get(&info, sizeof(info) - 4));
    PRINT("4 bytes over", get(&padded.info, sizeof(padded.info) + 4));

    static const SceUInt32 masks[3] = { SCE_KERNEL_CPU_MASK_USER_0, SCE_KERNEL_CPU_MASK_USER_1, SCE_KERNEL_CPU_MASK_USER_2 };
    for (int core = 0; core < 3; core++) {
        SceKernelSystemInfo before, after;
        get(&before, sizeof(before));
        const SceUID sleeper = sceKernelCreateThread("sleeper", sleeper_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, masks[core], NULL);
        sceKernelStartThread(sleeper, 0, NULL);
        test_join_thread(sleeper);
        sceKernelDelayThread(20000);
        get(&after, sizeof(after));
        PRINTF(after.cpuInfo[core].threadSwitchCount - before.cpuInfo[core].threadSwitchCount >= 10, "core %d: its thread switch count grew by at least 10", core);
        PRINTF(after.cpuInfo[core].idleClock > before.cpuInfo[core].idleClock, "core %d: its idle clock grew", core);
    }
    test_exit();
    return 0;
}
