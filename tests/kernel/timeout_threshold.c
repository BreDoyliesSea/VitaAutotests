// Up to which timeout a wait ends at once instead of going through the timer. A wait with a
// timeout of 1 microsecond ends after about 15, while one of 50 takes at least the about 240 a
// delay takes at the shortest (delay_precision). The main thread, on core 1 where nothing else
// runs, times 10 semaphore waits and 10 delays of each length with the process time, and prints
// whether every one ended within 100 microseconds, and whether every one took at least 100, far
// from both.
//
// On a PS Vita TV on firmware 3.65 only the wait with a timeout of 1 ends at once every time.
// One of 2 ends at once on some waits and goes through the timer on others, so the test leaves
// it out. From 4 up every wait, and every delay from 1 up, takes the timer's way.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define RUNS 10

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    const SceUID sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    // Not 2, which ends at once on some waits and goes through the timer on others
    static const SceUInt32 lengths[7] = { 1, 4, 8, 16, 32, 64, 100 };
    for (int delay = 0; delay < 2; delay++) {
        for (int l = 0; l < 7; l++) {
            int quick = 1, slow = 1;
            for (int i = 0; i < RUNS; i++) {
                const SceUInt64 start = sceKernelGetProcessTimeWide();
                if (delay) {
                    sceKernelDelayThread(lengths[l]);
                } else {
                    SceUInt32 timeout = lengths[l];
                    sceKernelWaitSema(sema, 1, &timeout);
                }
                const SceUInt64 took = sceKernelGetProcessTimeWide() - start;
                quick &= took < 100;
                slow &= took >= 100;
            }
            const char *const name = delay ? "a delay of" : "a wait with a timeout of";
            PRINTF(quick, "%s %u: every one ended within 100 microseconds", name, (unsigned)lengths[l]);
            PRINTF(slow, "%s %u: every one took at least 100 microseconds", name, (unsigned)lengths[l]);
        }
    }
    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}
