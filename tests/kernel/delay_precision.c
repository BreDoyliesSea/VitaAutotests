// How long sceKernelDelayThread and a wait's timeout take compared to what they ask for. The
// main thread, on core 1 where nothing else runs, times 20 delays of each length with the
// process time, and 20 semaphore waits with a timeout of each length, and prints whether every
// one took at least what it asked for, whether every one ended within 1 ms after that, and for
// the lengths under 150 microseconds whether every one took at least 150.
//
// On a PS Vita TV on firmware 3.65 every one took at least what it asked for and ended within 1
// ms after that. The delays under 150 microseconds and the timeouts of 50 and 100 took at least
// 150, about 240 at the shortest, while a wait with a timeout of 1 ended after about 15.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define RUNS 20
// Well below the shortest a short delay took on a Vita
#define FLOOR_US 150

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    const SceUID sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    static const SceUInt32 lengths[5] = { 1, 50, 100, 1000, 10000 };
    for (int wait = 0; wait < 2; wait++) {
        for (int l = 0; l < 5; l++) {
            int at_least = 1, within = 1, over_floor = 1;
            for (int i = 0; i < RUNS; i++) {
                const SceUInt64 start = sceKernelGetProcessTimeWide();
                if (wait) {
                    SceUInt32 timeout = lengths[l];
                    sceKernelWaitSema(sema, 1, &timeout);
                } else {
                    sceKernelDelayThread(lengths[l]);
                }
                const SceUInt64 took = sceKernelGetProcessTimeWide() - start;
                at_least &= took >= lengths[l];
                within &= took < lengths[l] + 1000;
                over_floor &= took >= FLOOR_US;
            }
            const char *const name = wait ? "a wait with a timeout of" : "a delay of";
            PRINTF(at_least, "%s %u: every one took at least that long", name, (unsigned)lengths[l]);
            PRINTF(within, "%s %u: every one ended within 1 ms after that", name, (unsigned)lengths[l]);
            if (lengths[l] < FLOOR_US)
                PRINTF(over_floor, "%s %u: every one took at least %u microseconds", name, (unsigned)lengths[l], FLOOR_US);
        }
    }
    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}
