// What a function a library calls back sees, next to what the kernel gives its own callbacks: the
// FPSCR, VFP exception setting and TPIDRURW in the host allocation callback that
// sceGxmShaderPatcherCreate calls, which thread it runs on, and whether the creating thread keeps
// the rounding mode and TPIDRURW the callback leaves.
#include <psp2/gxm.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>

#include "registers.h"
#include "test.h"
#include "threadmgr_extra.h"

static SceUID creator;
static volatile int host_allocs;
static volatile unsigned int cb_fpscr = 0xDEAD, cb_setting = 0xDEAD, cb_tpidrurw = 0xDEAD, cb_on_creator = 0xDEAD;

static void display_callback(const void *data) {
    (void)data;
}

static void *host_alloc(void *user_data, SceSize size) {
    (void)user_data;
    if (host_allocs++ == 0) {
        cb_fpscr = get_fpscr() & FPSCR_CONTROL;
        cb_setting = sceKernelGetCurrentThreadVfpException();
        cb_tpidrurw = get_tpidrurw();
        cb_on_creator = sceKernelGetThreadId() == creator;
        // Values of its own, to see whether the creating thread keeps them
        set_rmode(RMODE_ZERO);
        set_tpidrurw(0x2222);
    }
    return malloc(size);
}

static void host_free(void *user_data, void *mem) {
    (void)user_data;
    free(mem);
}

// No GPU memory, so a patcher that needs some at creation fails instead
static void *buffer_alloc(void *user_data, SceSize size) {
    (void)user_data;
    (void)size;
    return NULL;
}

static void *usse_alloc(void *user_data, SceSize size, unsigned int *usse_offset) {
    (void)user_data;
    (void)size;
    *usse_offset = 0;
    return NULL;
}

static void mem_free(void *user_data, void *mem) {
    (void)user_data;
    (void)mem;
}

int main(void) {
    creator = sceKernelGetThreadId();
    SceGxmInitializeParams init = { 0 };
    init.displayQueueMaxPendingCount = 2;
    init.displayQueueCallback = display_callback;
    init.displayQueueCallbackDataSize = 4;
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    PRINT("initialize libgxm", sceGxmInitialize(&init));

    // Values of the creating thread's own
    set_rmode(RMODE_PLUS);
    sceKernelChangeThreadVfpException(0, SCE_KERNEL_VFP_EXCP_QC);
    set_tpidrurw(0x1111);

    SceGxmShaderPatcherParams params = { 0 };
    params.hostAllocCallback = host_alloc;
    params.hostFreeCallback = host_free;
    params.bufferAllocCallback = buffer_alloc;
    params.bufferFreeCallback = mem_free;
    params.vertexUsseAllocCallback = usse_alloc;
    params.vertexUsseFreeCallback = mem_free;
    params.fragmentUsseAllocCallback = usse_alloc;
    params.fragmentUsseFreeCallback = mem_free;
    SceGxmShaderPatcher *patcher = NULL;
    const int created = sceGxmShaderPatcherCreate(&params, &patcher);
    const unsigned int after_rmode = get_rmode();
    const unsigned int after_setting = sceKernelGetCurrentThreadVfpException();
    const unsigned int after_tpidrurw = get_tpidrurw();
    sceKernelChangeThreadVfpException(SCE_KERNEL_VFP_EXCP_ALL, 0);

    PRINT("create a shader patcher", created);
    PRINT("the host allocation callback ran", host_allocs > 0);
    PRINT("it runs on the creating thread", cb_on_creator);
    PRINT("it sees the FPSCR control bits", cb_fpscr);
    PRINT("it sees the VFP exception setting", cb_setting);
    PRINT("it sees TPIDRURW", cb_tpidrurw);
    PRINT("after the create: the creating thread's rounding mode", after_rmode);
    PRINT("after the create: its VFP exception setting", after_setting);
    PRINT("after the create: its TPIDRURW", after_tpidrurw);
    if (created == 0)
        PRINT("destroy the shader patcher", sceGxmShaderPatcherDestroy(patcher));
    PRINT("terminate libgxm", sceGxmTerminate());

    test_exit();
    return 0;
}
