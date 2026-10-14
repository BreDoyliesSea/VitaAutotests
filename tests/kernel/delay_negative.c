// A delay of -1, which as an unsigned delay is over an hour long, in its own test since a thread
// still in it is left for the process exit to end. Whether the call fails or the thread waits.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static volatile int delay_result = 0xDEAD, delay_done;

static int delay_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    delay_result = sceKernelDelayThread((SceUInt32)-1);
    delay_done = 1;
    return 0;
}

static SceUInt32 wait_type(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 ? info.waitType : 0;
}

int main(void) {
    const SceUID thread = test_start_thread(delay_thread);
    TEST_WAIT_FOR(wait_type(thread) != 0 || delay_done);
    PRINT("a delay of -1: the wait type", wait_type(thread));
    // A delay that doesn't block for long ends well within this
    sceKernelDelayThread(100000);
    PRINT("a delay of -1: still delaying after 100 ms", wait_type(thread) == WAIT_TYPE_DELAY);
    PRINT("a delay of -1: it ended", delay_done);
    if (delay_done) {
        PRINT("a delay of -1: its result", delay_result);
        sceKernelDeleteThread(thread);
    }

    test_exit();
    return 0;
}
