// Which thread runs a module's start and stop functions when a thread loads and unloads it: the
// calling thread or another one, at what priority, on which stack, and whether the arguments are
// passed as they are or copied. probe_module records what it sees through a pointer in its
// arguments. Addresses are printed as comparisons.
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>

#include "probe_module/probe_module.h"
#include "test.h"

#define MODULE_PATH "app0:probe_module.suprx"
#define THREAD_PRIORITY 0x70

static ModuleProbe start_probe, stop_probe;
static SceUID main_start_thread;
static volatile int start_thread_alive;
static volatile int load_result, start_status, unload_result, stop_status;
static volatile SceUIntPtr caller_frame, start_args, stop_args;
static volatile SceUID caller;
static volatile int caller_priority;

static void load_and_unload(void) {
    volatile int local = 0;
    caller_frame = (SceUIntPtr)&local;
    caller = sceKernelGetThreadId();
    caller_priority = sceKernelGetThreadCurrentPriority();
    sceClibMemset(&start_probe, 0, sizeof(start_probe));
    sceClibMemset(&stop_probe, 0, sizeof(stop_probe));
    ModuleArgs start = { &start_probe };
    ModuleArgs stop = { &stop_probe };
    start_args = (SceUIntPtr)&start;
    stop_args = (SceUIntPtr)&stop;
    int status = 0xDEAD;
    const SceUID module = sceKernelLoadStartModule(MODULE_PATH, sizeof(start), &start, 0, NULL, &status);
    load_result = test_created(module);
    start_status = status;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    start_thread_alive = start_probe.runs && sceKernelGetThreadInfo(start_probe.thread, &info) == 0;
    status = 0xDEAD;
    unload_result = module >= 0 ? sceKernelStopUnloadModule(module, sizeof(stop), &stop, 0, NULL, &status) : module;
    stop_status = status;
}

static int loading_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    load_and_unload();
    return 0;
}

static void print_probe(const char *name, const char *what, const ModuleProbe *probe, SceUIntPtr args) {
    PRINTF(probe->runs, "%s: runs of its %s function", name, what);
    if (!probe->runs)
        return;
    PRINTF(probe->thread == caller, "%s: its %s function runs on the calling thread", name, what);
    PRINTF(probe->process == sceKernelGetProcessId(), "%s: its thread is in this process", name);
    PRINTF(probe->priority, "%s: its thread's priority", name);
    PRINTF(probe->priority == caller_priority, "%s: that is the caller's priority", name);
    PRINTF(probe->init_priority, "%s: its thread's initial priority", name);
    PRINTF(probe->stack_size, "%s: its thread's stack size", name);
    PRINTF(probe->attr, "%s: its thread's attributes", name);
    PRINTF(probe->frame < caller_frame && caller_frame - probe->frame < 0x10000, "%s: on the caller's stack, below its frame", name);
    PRINTF(probe->argp == args, "%s: it gets the caller's arguments as they are", name);
    PRINTF(probe->args, "%s: their size", name);
}

static void print_all(const char *name) {
    PRINTF(load_result, "%s: load and start", name);
    PRINTF(start_status, "%s: the start status", name);
    print_probe(name, "start", &start_probe, start_args);
    PRINTF(start_thread_alive, "%s: the thread of the start function is still there after the load", name);
    PRINTF(unload_result, "%s: stop and unload", name);
    PRINTF(stop_status, "%s: the stop status", name);
    print_probe(name, "stop", &stop_probe, stop_args);
    PRINTF(start_probe.runs && stop_probe.runs && start_probe.thread == stop_probe.thread, "%s: start and stop run on the same thread", name);
}

int main(void) {
    load_and_unload();
    main_start_thread = start_probe.thread;
    print_all("main thread");

    const SceUID thread = sceKernelCreateThread("loader", loading_thread, THREAD_PRIORITY, 0x4000, 0, 0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    test_join_thread(thread);
    print_all("another thread");
    PRINT("both loads run the start function on the same thread", start_probe.thread == main_start_thread);

    test_exit();
    return 0;
}
