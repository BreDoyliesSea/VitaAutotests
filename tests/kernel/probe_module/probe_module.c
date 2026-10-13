// A module whose start and stop functions record the thread they run on, for module_start_thread
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>

#include "probe_module.h"

static void record(SceSize args, void *argp) {
    if (args < sizeof(ModuleArgs) || !argp)
        return;
    ModuleProbe *probe = ((ModuleArgs *)argp)->out;
    volatile int local = 0;
    probe->thread = sceKernelGetThreadId();
    probe->process = sceKernelGetProcessId();
    probe->priority = sceKernelGetThreadCurrentPriority();
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(probe->thread, &info);
    probe->init_priority = info.initPriority;
    probe->stack_size = info.stackSize;
    probe->attr = info.attr;
    probe->frame = (SceUIntPtr)&local;
    probe->argp = (SceUIntPtr)argp;
    probe->args = args;
    probe->runs++;
}

int module_start(SceSize args, void *argp) {
    record(args, argp);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
    record(args, argp);
    return SCE_KERNEL_STOP_SUCCESS;
}
