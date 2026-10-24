// What the thread manager calls return inside a thread event handler, and thread event handler
// registrations with unusual thread and mask arguments.
//
// First, a handler registered for a fresh target thread with the START and END mask, which acts
// only on the run of one type. In a START run, then in an END run, it registers another handler for
// the same thread, unregisters its own handler, creates a thread (deleting it if it exists) and
// starts a thread the main thread created before. The results are printed after the target ended,
// with how often the probing run happened, how often the handler registered inside it ran, whether
// the thread it started ran, and the result of unregistering the handler from the main thread.
//
// Then a handler for the END event only that returns 1, on a target started twice, with how often
// it ran after each run of the target and the result of unregistering it from the main thread.
//
// Last, registrations from the main thread: for every thread with the mask 0, for a created thread
// that never starts with the mask 0, for the calling thread given as 0 with START and with END, for
// the calling thread's own UID with END, and for every thread with a mask bit that isn't START or
// END. Each handler that was created is unregistered at once.
//
// On a PS Vita TV on firmware 3.65 all four calls in a handler's run refuse with 0x80027101, the
// same in the START and the END run. The thread the handler tried to start never runs, and the
// handler registered by the main thread stays registered, so unregistering it from there gives 0.
// The END handler that returns 1 runs only on the first run, and unregistering it after gives
// 0x80028061, so its return unregistered it. Registering for every thread or for a created thread with the mask 0 works, the calling thread
// given as 0 with START gives 0x80020005 and with END works, and every thread with the mask 0x10
// gives 0x80020005.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

static SceUID target, own_handler, nested_handler_id, started_thread;
static volatile int probe_type, probe_runs, nested_runs, started_ran;
static volatile int register_result, unregister_result, create_result, start_result;

static int nested_handler(SceInt32 type, SceUID thread_id, SceInt32 arg, void *common) {
    (void)type;
    (void)thread_id;
    (void)arg;
    (void)common;
    nested_runs++;
    return 0;
}

static int small_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    started_ran = 1;
    return 0;
}

// Does nothing, for the registrations of the main thread
static int plain_handler(SceInt32 type, SceUID thread_id, SceInt32 arg, void *common) {
    (void)type;
    (void)thread_id;
    (void)arg;
    (void)common;
    return 0;
}

// On the run of the type probe_type, registers, unregisters, creates and starts
static int handler(SceInt32 type, SceUID thread_id, SceInt32 arg, void *common) {
    (void)thread_id;
    (void)arg;
    (void)common;
    if (type != probe_type)
        return 0;
    probe_runs++;
    nested_handler_id = sceKernelRegisterThreadEventHandler("nested", target, SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, nested_handler, NULL);
    register_result = test_created(nested_handler_id);
    unregister_result = sceKernelUnregisterThreadEventHandler(own_handler);
    const SceUID created = sceKernelCreateThread("small", small_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    create_result = test_created(created);
    if (created >= 0)
        sceKernelDeleteThread(created);
    start_result = sceKernelStartThread(started_thread, 0, NULL);
    return 0;
}

static int target_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

static void run(const char *name, int type) {
    probe_type = type;
    probe_runs = nested_runs = started_ran = 0;
    register_result = unregister_result = create_result = start_result = 0xDEAD;
    nested_handler_id = -1;

    target = sceKernelCreateThread("target", target_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    started_thread = sceKernelCreateThread("started", small_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    own_handler = sceKernelRegisterThreadEventHandler("handler", target, SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, handler, NULL);
    PRINTF(test_created(own_handler), "%s: register the handler", name);
    sceKernelStartThread(target, 0, NULL);
    SceUInt timeout = TEST_WAIT_US;
    PRINTF(sceKernelWaitThreadEnd(target, NULL, &timeout), "%s: the wait for the end of the target", name);

    // Waits for the thread the handler started
    if (start_result == 0) {
        timeout = TEST_WAIT_US;
        sceKernelWaitThreadEnd(started_thread, NULL, &timeout);
    }
    PRINTF(probe_runs, "%s: runs of the handler for that type", name);
    PRINTF(register_result, "%s: register a handler for the same thread", name);
    PRINTF(unregister_result, "%s: unregister the handler itself", name);
    PRINTF(create_result, "%s: create a thread", name);
    PRINTF(start_result, "%s: start a thread created before", name);
    PRINTF(started_ran, "%s: the thread it started ran", name);
    PRINTF(nested_runs, "%s: runs of the handler registered inside", name);
    PRINTF(sceKernelUnregisterThreadEventHandler(own_handler), "%s: unregister the handler from the main thread", name);
    if (nested_handler_id >= 0)
        sceKernelUnregisterThreadEventHandler(nested_handler_id);
    sceKernelDeleteThread(started_thread);
    sceKernelDeleteThread(target);
}

static volatile int end_runs;

// Counts its END runs and returns 1
static int returning_handler(SceInt32 type, SceUID thread_id, SceInt32 arg, void *common) {
    (void)type;
    (void)thread_id;
    (void)arg;
    (void)common;
    end_runs++;
    return 1;
}

// Starts the target, waits for its end and prints the runs of the END handler
static void run_target(const char *name) {
    sceKernelStartThread(target, 0, NULL);
    SceUInt timeout = TEST_WAIT_US;
    PRINTF(sceKernelWaitThreadEnd(target, NULL, &timeout), "%s: the wait for the end", name);
    PRINTF(end_runs, "%s: runs of the handler", name);
}

// Registers a plain handler and unregisters it again if that worked
static void register_for(const char *name, SceUID thread, SceInt32 mask) {
    const SceUID id = sceKernelRegisterThreadEventHandler("plain", thread, mask, plain_handler, NULL);
    PRINTF(test_created(id), "register for %s", name);
    if (id >= 0)
        sceKernelUnregisterThreadEventHandler(id);
}

int main(void) {
    run("in a start handler", SCE_KERNEL_THREAD_EVENT_TYPE_START);
    run("in an end handler", SCE_KERNEL_THREAD_EVENT_TYPE_EXIT);

    // An END handler that returns 1, with the target started twice
    target = sceKernelCreateThread("target", target_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    const SceUID returning = sceKernelRegisterThreadEventHandler("returning", target, SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, returning_handler, NULL);
    PRINT("an end handler that returns 1: register", test_created(returning));
    run_target("an end handler that returns 1: first run");
    run_target("an end handler that returns 1: second run");
    PRINT("an end handler that returns 1: unregister it after", sceKernelUnregisterThreadEventHandler(returning));
    sceKernelDeleteThread(target);

    const SceUID idle = sceKernelCreateThread("idle", target_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    register_for("every thread with the mask 0", SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID, 0);
    register_for("a created thread with the mask 0", idle, 0);
    register_for("the calling thread as 0 with START", 0, SCE_KERNEL_THREAD_EVENT_TYPE_START);
    register_for("the calling thread as 0 with END", 0, SCE_KERNEL_THREAD_EVENT_TYPE_EXIT);
    register_for("the calling thread by UID with END", sceKernelGetThreadId(), SCE_KERNEL_THREAD_EVENT_TYPE_EXIT);
    register_for("every thread with the mask 0x10", SCE_KERNEL_THREAD_ID_PROCESS_ALL_ID, 0x10);
    sceKernelDeleteThread(idle);

    test_exit();
    return 0;
}
