// A module whose start and stop functions record the thread they run on, for module_start_thread
// and module_start_vfp
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>

#include "probe_module.h"
#include "registers.h"
#include "threadmgr_extra.h"

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
    probe->fpscr = get_fpscr() & FPSCR_CONTROL;
    probe->vfp_setting = sceKernelGetCurrentThreadVfpException();
    probe->tpidrurw = get_tpidrurw();
    // Values of its own, which the stop function and the caller shouldn't see
    set_fpscr(get_fpscr() & ~FPSCR_FLAGS);
    set_rmode(RMODE_MINUS);
    sceKernelChangeThreadVfpException(0, SCE_KERNEL_VFP_EXCP_DZC);
    set_tpidrurw(0x4444);
}

int module_start(SceSize args, void *argp) {
    record(args, argp);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
    record(args, argp);
    return SCE_KERNEL_STOP_SUCCESS;
}
