// The threads that run module start and stop functions, seen by a thread event handler registered
// for every thread of the process: their name, stack size, attributes and priority when a module is
// loaded and unloaded, for the test's own module and for firmware modules loaded through the
// sysmodule calls, which may come with start parameters of their own.
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>

#include "test.h"

#define THREAD_EVENT_START 0x4
// Stands for every thread of the process, instead of one thread's UID
#define ALL_USER_THREADS 0x10027
#define MODULE_PATH "app0:probe_module.suprx"
#define MAX_STARTS 8

typedef struct Start {
    char name[32];
    unsigned int stack_size, attr, priority, on_it;
} Start;

static Start starts[MAX_STARTS];
static volatile int start_count;

static int handler(SceInt32 type, SceUID thread_id, SceInt32 arg, void *common) {
    (void)type;
    (void)arg;
    (void)common;
    const int i = start_count;
    if (i >= MAX_STARTS)
        return 0;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread_id, &info);
    sceClibStrncpy(starts[i].name, info.name, sizeof(starts[i].name) - 1);
    starts[i].stack_size = info.stackSize;
    starts[i].attr = info.attr;
    starts[i].priority = info.currentPriority;
    starts[i].on_it = sceKernelGetThreadId() == thread_id;
    start_count = i + 1;
    return 0;
}

static void print_starts(const char *name, int result) {
    PRINTF(result, "%s", name);
    PRINTF(start_count, "%s: threads that started", name);
    for (int i = 0; i < start_count && i < MAX_STARTS; i++) {
        PRINTF(1, "%s: thread %d is named %s", name, i + 1, starts[i].name);
        PRINTF(starts[i].stack_size, "%s: thread %d: its stack size", name, i + 1);
        PRINTF(starts[i].attr, "%s: thread %d: its attributes", name, i + 1);
        PRINTF(starts[i].priority, "%s: thread %d: its priority", name, i + 1);
        PRINTF(starts[i].on_it, "%s: thread %d: the handler runs on it", name, i + 1);
    }
    sceClibMemset(starts, 0, sizeof(starts));
    start_count = 0;
}

static void sysmodule(const char *name, SceSysmoduleModuleId id) {
    char label[64];
    sceClibSnprintf(label, sizeof(label), "load %s", name);
    print_starts(label, sceSysmoduleLoadModule(id));
    sceClibSnprintf(label, sizeof(label), "unload %s", name);
    print_starts(label, sceSysmoduleUnloadModule(id));
}

int main(void) {
    const SceUID id = sceKernelRegisterThreadEventHandler("starts", ALL_USER_THREADS, THREAD_EVENT_START, handler, NULL);
    PRINT("register the handler", test_created(id));

    int status = 0;
    const SceUID module = sceKernelLoadStartModule(MODULE_PATH, 0, NULL, 0, NULL, &status);
    print_starts("load and start the test's module", test_created(module));
    print_starts("stop and unload it", module >= 0 ? sceKernelStopUnloadModule(module, 0, NULL, 0, NULL, &status) : module);
    sysmodule("libfiber", SCE_SYSMODULE_FIBER);
    // A firmware module without a start function
    sysmodule("libjson", SCE_SYSMODULE_JSON);

    if (id >= 0)
        sceKernelUnregisterThreadEventHandler(id);
    test_exit();
    return 0;
}
