// A thread other than the main one ends the process, while the main thread and another thread
// wait. If the exit works, nothing prints after DONE. The main thread only prints again if the
// process is still there 10 s later.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

static SceUID sema, never;

static int exiting_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    PRINT("the thread runs", 1);
    test_exit();
    PRINT("the exit returned", 1);
    return 0;
}

static int waiting_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 4 * TEST_WAIT_US;
    PRINT("the other thread's wait returned", sceKernelWaitSemaCB(sema, 1, &timeout));
    return 0;
}

int main(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    never = sceKernelCreateSema("never", 0, 0, 1, NULL);
    const SceUID waiting = test_start_thread(waiting_thread);
    test_wait_until_waiting(waiting);
    test_start_thread(exiting_thread);
    SceUInt timeout = 5 * TEST_WAIT_US;
    PRINT("the process is still there", sceKernelWaitSema(never, 1, &timeout));
    test_exit();
    return 0;
}
