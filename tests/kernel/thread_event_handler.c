// Thread event handlers: whether registering one works, and if so when it runs for a thread's start
// and end, with which arguments, and on which thread
#include <psp2/kernel/threadmgr.h>

#include "test.h"

static int common_marker;
static SceUID target;
static volatile int log_type[8], log_thread[8], log_arg[8], log_common[8], log_on_target[8], log_count;
static volatile int entry_ran_after;

static int handler(SceInt32 type, SceUID thread_id, SceInt32 arg, void *common) {
    if (log_count < 8) {
        log_type[log_count] = type;
        log_thread[log_count] = thread_id == target;
        log_arg[log_count] = arg;
        log_common[log_count] = common == &common_marker;
        log_on_target[log_count] = sceKernelGetThreadId() == target;
        log_count++;
    }
    return 0;
}

static int returns_7(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    entry_ran_after = log_count;
    return 7;
}

static int exits_9(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    entry_ran_after = log_count;
    sceKernelExitThread(9);
    return 1;
}

static void print_log(const char *name) {
    PRINTF(log_count, "%s: handler runs", name);
    PRINTF(entry_ran_after, "%s: handler runs before the entry", name);
    for (int i = 0; i < log_count; i++) {
        PRINTF(log_type[i], "%s: run %d: the type", name, i + 1);
        PRINTF(log_thread[i], "%s: run %d: the thread is the target", name, i + 1);
        PRINTF(log_arg[i], "%s: run %d: the argument", name, i + 1);
        PRINTF(log_common[i], "%s: run %d: the common pointer is the one given", name, i + 1);
        PRINTF(log_on_target[i], "%s: run %d: it runs on the target", name, i + 1);
    }
}

static void run(const char *name, SceKernelThreadEntry entry, SceUID registered_for) {
    log_count = 0;
    entry_ran_after = -1;
    target = sceKernelCreateThread("target", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    const SceUID id = sceKernelRegisterThreadEventHandler("handler", registered_for ? registered_for : target, SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, handler, &common_marker);
    PRINTF(test_created(id), "%s: register", name);
    sceKernelStartThread(target, 0, NULL);
    int status = 0xDEAD;
    sceKernelWaitThreadEnd(target, &status, NULL);
    PRINTF(status, "%s: the exit status", name);
    print_log(name);
    if (id >= 0)
        PRINTF(sceKernelUnregisterThreadEventHandler(id), "%s: unregister", name);
    sceKernelDeleteThread(target);
}

int main(void) {
    run("a thread that returns", returns_7, 0);
    run("a thread that exits", exits_9, 0);
    run("every thread", returns_7, SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID);
    PRINT("register for an unknown thread", test_created(sceKernelRegisterThreadEventHandler("handler", 0x12345, SCE_KERNEL_THREAD_EVENT_TYPE_START, handler, NULL)));
    PRINT("register for no events", test_created(sceKernelRegisterThreadEventHandler("handler", sceKernelGetThreadId(), 0, handler, NULL)));
    PRINT("unregister an unknown handler", sceKernelUnregisterThreadEventHandler(0x12345));

    test_exit();
    return 0;
}
