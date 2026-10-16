// What sceGxmMapMemory does with a range that's in no memory block: 64 KiB where a block was freed.
// It's apart from gxm_map_memory since a wrong implementation may crash on it, mapping memory that
// isn't there or touching it on the unmap. A map that works is unmapped again.
//
// On a PS Vita TV on firmware 3.65 the map fails with 0x805B0017 (SCE_GXM_ERROR_DRIVER).
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400

static void display_callback(const void *data) {
    (void)data;
}

int main(void) {
    SceGxmInitializeParams init = { 0 };
    init.displayQueueMaxPendingCount = 2;
    init.displayQueueCallback = display_callback;
    init.displayQueueCallbackDataSize = 4;
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    PRINT("initialize libgxm", sceGxmInitialize(&init));

    const SceUID block = sceKernelAllocMemBlock("gone", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 64 * KIB, NULL);
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    sceKernelFreeMemBlock(block);
    const int mapped = sceGxmMapMemory(base, 64 * KIB, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    PRINT("map a range in no block", mapped);
    if (mapped == 0)
        PRINT("unmap it", sceGxmUnmapMemory(base));
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
