// What TLS slot 0 holds next to the process ID: whether the process ID is 0, and whether slot 0
// equals it in the main thread, and in a thread created before and one created after the process
// ID was first read, at the thread's start and after it calls sceKernelGetProcessId itself. Also
// the process ID thread info reports. Only comparisons are printed, never the ID.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

static SceUInt32 slot0(void) {
    return *(volatile SceUInt32 *)sceKernelGetTLSAddr(0);
}

static volatile SceUInt32 before, after;
static volatile SceUID pid_in_thread;

static int thread_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    before = slot0();
    pid_in_thread = sceKernelGetProcessId();
    after = slot0();
    return 0;
}

static SceUID create(void) {
    return sceKernelCreateThread("slot0", thread_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
}

static void run_thread(const char *name, SceUID thread, SceUID pid) {
    before = after = 0xDEAD;
    sceKernelStartThread(thread, 0, NULL);
    test_join_thread(thread);
    PRINTF(before == 0, "%s: slot 0 at its start is 0", name);
    PRINTF(before == (SceUInt32)pid, "%s: slot 0 at its start is the process ID", name);
    PRINTF(after == (SceUInt32)pid, "%s: slot 0 after it read the process ID is the process ID", name);
    PRINTF(pid_in_thread == pid, "%s: it reads the same process ID", name);
}

int main(void) {
    const SceUID early = create();
    const SceUInt32 main_before = slot0();
    const SceUID pid = sceKernelGetProcessId();
    const SceUInt32 main_after = slot0();
    PRINT("the process ID is 0", pid == 0);
    PRINT("the process ID is positive", pid > 0);
    PRINT("main thread: slot 0 before reading the process ID is 0", main_before == 0);
    PRINT("main thread: slot 0 before reading the process ID is the process ID", main_before == (SceUInt32)pid);
    PRINT("main thread: slot 0 after reading the process ID is the process ID", main_after == (SceUInt32)pid);

    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    PRINT("main thread: thread info reports the process ID", info.processId == pid);

    run_thread("a thread created before the process ID was read", early, pid);
    run_thread("a thread created after it was read", create(), pid);

    test_exit();
    return 0;
}
