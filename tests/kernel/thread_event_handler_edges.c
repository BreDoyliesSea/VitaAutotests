// Thread event handlers in less common cases: a thread that exits and deletes itself, a handler
// for every thread registered before the thread exists, one registered after the thread started,
// two handlers on one thread, start handlers that return something else than 0 or unregister
// themselves, a start only mask, and a thread started twice. Each handler run logs what the thread
// reports about itself.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

enum Entry { RETURNS, EXIT_DELETES, WAITS };

static SceUID target, go, self_handler;
static volatile int log_count, entry_runs, handler_return, unregister_self;
static volatile int log_handler[16], log_type[16], log_status[16], log_exit_status[16], log_entry_runs[16];

// Logs a run for the target. The common pointer holds the handler's number.
static int handler(SceInt32 type, SceUID thread_id, SceInt32 arg, void *common) {
    (void)arg;
    if (thread_id != target)
        return 0;
    if (log_count < 16) {
        SceKernelThreadInfo info = { .size = sizeof(info) };
        sceKernelGetThreadInfo(thread_id, &info);
        const int i = log_count++;
        log_handler[i] = (int)(SceUIntPtr)common;
        log_type[i] = type;
        log_status[i] = info.status;
        log_exit_status[i] = info.exitStatus;
        log_entry_runs[i] = entry_runs;
    }
    if (unregister_self && type == SCE_KERNEL_THREAD_EVENT_TYPE_START)
        sceKernelUnregisterThreadEventHandler(self_handler);
    return type == SCE_KERNEL_THREAD_EVENT_TYPE_START ? handler_return : 0;
}

static int target_thread(SceSize args, void *argp) {
    (void)args;
    entry_runs++;
    switch (*(const int *)argp) {
    case EXIT_DELETES: sceKernelExitDeleteThread(6); break;
    case WAITS: {
        SceUInt timeout = TEST_WAIT_US;
        sceKernelWaitSema(go, 1, &timeout);
        break;
    }
    }
    return 5;
}

static SceUID create_target(void) {
    target = sceKernelCreateThread("target", target_thread, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    return target;
}

static void start_target(int entry) {
    sceKernelStartThread(target, sizeof(entry), &entry);
}

static SceUID add(SceUID thread, SceInt32 mask, int number) {
    return sceKernelRegisterThreadEventHandler("handler", thread, mask, handler, (void *)(SceUIntPtr)number);
}

static void reset(void) {
    log_count = entry_runs = handler_return = unregister_self = 0;
}

static void end_status(const char *name) {
    int status = 0xDEAD;
    SceUInt timeout = TEST_WAIT_US;
    PRINTF(sceKernelWaitThreadEnd(target, &status, &timeout), "%s: the wait for the end", name);
    PRINTF(status, "%s: the exit status", name);
}

static void print_log(const char *name) {
    PRINTF(log_count, "%s: handler runs", name);
    PRINTF(entry_runs, "%s: runs of the entry", name);
    for (int i = 0; i < log_count; i++) {
        PRINTF(log_handler[i], "%s: run %d: the handler", name, i + 1);
        PRINTF(log_type[i], "%s: run %d: the type", name, i + 1);
        PRINTF(log_status[i], "%s: run %d: the thread's status", name, i + 1);
        PRINTF(log_exit_status[i], "%s: run %d: the thread's exit status", name, i + 1);
        PRINTF(log_entry_runs[i], "%s: run %d: runs of the entry before it", name, i + 1);
    }
}

static int thread_gone(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) < 0;
}

int main(void) {
    go = sceKernelCreateSema("go", 0, 0, 1, NULL);
    SceUID id;

    // A thread that exits and deletes itself
    reset();
    create_target();
    id = add(target, SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, 1);
    PRINT("exit and delete: register", test_created(id));
    start_target(EXIT_DELETES);
    TEST_WAIT_FOR(thread_gone(target));
    print_log("exit and delete");
    PRINT("exit and delete: unregister", sceKernelUnregisterThreadEventHandler(id));

    // A handler for every thread, registered before the thread is created
    reset();
    id = add(SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID, SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, 1);
    PRINT("every thread, created after: register", test_created(id));
    create_target();
    start_target(RETURNS);
    end_status("every thread, created after");
    print_log("every thread, created after");
    sceKernelUnregisterThreadEventHandler(id);
    sceKernelDeleteThread(target);

    // A handler registered while the thread already runs
    reset();
    create_target();
    start_target(WAITS);
    test_wait_until_waiting(target);
    id = add(target, SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, 1);
    PRINT("registered after the start: register", test_created(id));
    sceKernelSignalSema(go, 1);
    end_status("registered after the start");
    print_log("registered after the start");
    sceKernelUnregisterThreadEventHandler(id);
    sceKernelDeleteThread(target);

    // Two handlers on one thread
    reset();
    create_target();
    const SceUID first = add(target, SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, 1);
    const SceUID second = add(target, SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, 2);
    PRINT("two handlers: register the first", test_created(first));
    PRINT("two handlers: register the second", test_created(second));
    start_target(RETURNS);
    end_status("two handlers");
    print_log("two handlers");
    sceKernelUnregisterThreadEventHandler(first);
    sceKernelUnregisterThreadEventHandler(second);
    sceKernelDeleteThread(target);

    // Start handlers that return 1 and -1
    static const int returns[] = { 1, -1 };
    for (int r = 0; r < 2; r++) {
        char name[48];
        sceClibSnprintf(name, sizeof(name), "a start handler that returns %d", returns[r]);
        reset();
        handler_return = returns[r];
        create_target();
        id = add(target, SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, 1);
        start_target(RETURNS);
        end_status(name);
        print_log(name);
        PRINTF(sceKernelUnregisterThreadEventHandler(id), "%s: unregister it after", name);
        sceKernelDeleteThread(target);
    }

    // A start handler that unregisters itself
    reset();
    unregister_self = 1;
    create_target();
    self_handler = add(target, SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, 1);
    start_target(RETURNS);
    end_status("unregistered in the start handler");
    print_log("unregistered in the start handler");
    PRINT("unregistered in the start handler: unregister it again", sceKernelUnregisterThreadEventHandler(self_handler));
    sceKernelDeleteThread(target);

    // Only the start event in the mask
    reset();
    create_target();
    id = add(target, SCE_KERNEL_THREAD_EVENT_TYPE_START, 1);
    start_target(RETURNS);
    end_status("start only");
    print_log("start only");
    sceKernelUnregisterThreadEventHandler(id);
    sceKernelDeleteThread(target);

    // A thread started twice
    reset();
    create_target();
    id = add(target, SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, 1);
    start_target(RETURNS);
    end_status("started twice, first run");
    start_target(RETURNS);
    end_status("started twice, second run");
    print_log("started twice");
    sceKernelUnregisterThreadEventHandler(id);
    sceKernelDeleteThread(target);

    test_exit();
    return 0;
}
