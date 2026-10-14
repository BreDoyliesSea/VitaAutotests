// What a module's start and stop functions start with: the FPSCR, VFP exception setting and
// TPIDRURW on the thread they run on, when the loading thread has values of its own and the start
// function leaves others, and whether the loading thread keeps its own. probe_module records them.
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>

#include "probe_module/probe_module.h"
#include "registers.h"
#include "test.h"
#include "threadmgr_extra.h"

#define MODULE_PATH "app0:probe_module.suprx"

static ModuleProbe start_probe, stop_probe;

static void print_probe(const char *what, const ModuleProbe *probe) {
    PRINTF(probe->runs, "the %s function: runs", what);
    PRINTF(probe->fpscr, "the %s function: the FPSCR control bits", what);
    PRINTF(probe->vfp_setting, "the %s function: the VFP exception setting", what);
    PRINTF(probe->tpidrurw, "the %s function: TPIDRURW", what);
}

int main(void) {
    // Values of the loading thread's own
    set_rmode(RMODE_PLUS);
    sceKernelChangeThreadVfpException(0, SCE_KERNEL_VFP_EXCP_QC);
    set_tpidrurw(0x7777);

    ModuleArgs start = { &start_probe };
    ModuleArgs stop = { &stop_probe };
    int status = 0xDEAD;
    const SceUID module = sceKernelLoadStartModule(MODULE_PATH, sizeof(start), &start, 0, NULL, &status);
    const int after_start_rmode = get_rmode();
    const int after_start_setting = sceKernelGetCurrentThreadVfpException();
    const unsigned int after_start_tpidrurw = get_tpidrurw();
    const int unload = module >= 0 ? sceKernelStopUnloadModule(module, sizeof(stop), &stop, 0, NULL, &status) : module;

    PRINT("load and start the module", test_created(module));
    print_probe("start", &start_probe);
    PRINT("after the start: the loading thread's rounding mode", after_start_rmode);
    PRINT("after the start: its VFP exception setting", after_start_setting);
    PRINT("after the start: its TPIDRURW", after_start_tpidrurw);
    PRINT("stop and unload the module", unload);
    print_probe("stop", &stop_probe);

    test_exit();
    return 0;
}
