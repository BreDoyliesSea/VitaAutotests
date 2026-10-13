// Starting a thread again after it ended: what its TLS slots, arguments, stack and exit status
// hold in the new run, and what start and delete do to a thread that hasn't ended
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID go;
static volatile int runs, slot_at_start, arg_value, arg_size, free_size[2];

static int restart_thread(SceSize args, void *argp) {
    runs++;
    SceUInt32 *slot = sceKernelGetTLSAddr(0);
    slot_at_start = slot ? (int)*slot : -1;
    if (slot)
        *slot = 0x1230 + runs;
    arg_size = args;
    arg_value = args >= sizeof(int) ? *(const int *)argp : -1;
    if (runs <= 2)
        free_size[runs - 1] = sceKernelGetThreadStackFreeSize(0);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(go, 1, &timeout);
    return runs;
}

int main(void) {
    go = sceKernelCreateSema("go", 0, 0, 1, NULL);
    const SceUID thread = sceKernelCreateThread("restart", restart_thread, 0x10000100, 0x4000, 0, 0, NULL);
    int arg = 0x11;
    sceKernelStartThread(thread, sizeof(arg), &arg);
    test_wait_until_waiting(thread);

    // While it waits
    PRINT("first run: its TLS slot at the start", slot_at_start);
    PRINT("first run: its argument", arg_value);
    PRINT("start the waiting thread", sceKernelStartThread(thread, 0, NULL));
    PRINT("delete the waiting thread", sceKernelDeleteThread(thread));
    int status = 0xDEAD;
    PRINT("its exit status while it waits", sceKernelGetThreadExitStatus(thread, &status));
    PRINT("the status written", status);
    sceKernelSignalSema(go, 1);
    status = 0xDEAD;
    PRINT("wait for the end of the first run", sceKernelWaitThreadEnd(thread, &status, NULL));
    PRINT("the first run's status", status);
    const SceUInt32 *slot = sceKernelGetThreadTLSAddr(thread, 0);
    PRINT("its TLS slot by its UID after it ended", slot ? (int)*slot : -1);

    // The second run, with another argument
    arg = 0x22;
    PRINT("start it again", sceKernelStartThread(thread, sizeof(arg), &arg));
    test_wait_until_waiting(thread);
    PRINT("second run: its TLS slot at the start", slot_at_start);
    PRINT("second run: its argument", arg_value);
    PRINT("second run: its stack free size is the same", free_size[1] == free_size[0]);
    status = 0xDEAD;
    PRINT("second run: its exit status", sceKernelGetThreadExitStatus(thread, &status));
    PRINT("second run: the status written", status);
    status = 0xDEAD;
    SceUInt timeout = 1000;
    PRINT("second run: a wait for its end that times out", sceKernelWaitThreadEnd(thread, &status, &timeout));
    PRINT("second run: the status written", status);
    sceKernelSignalSema(go, 1);
    status = 0xDEAD;
    sceKernelWaitThreadEnd(thread, &status, NULL);
    PRINT("the second run's status", status);

    // A third run with no arguments
    PRINT("start it with no arguments", sceKernelStartThread(thread, 0, NULL));
    test_wait_until_waiting(thread);
    PRINT("third run: its argument size", arg_size);
    sceKernelSignalSema(go, 1);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    PRINT("delete it", sceKernelDeleteThread(thread));

    test_exit();
    return 0;
}
