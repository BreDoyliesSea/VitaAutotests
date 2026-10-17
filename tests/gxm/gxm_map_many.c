// How many sceGxmMapMemory mappings libgxm holds at once: 4 KiB mappings one after another across a
// 16 MiB block, up to 4096 of them, until one fails, with whether at least 64, 256 and 1024 and
// all of them worked, the error of the one that failed, and whether every unmap works. The count
// itself isn't printed, in case it depends on what else the system has mapped. Then 32 MiB mapped
// in 1 MiB pieces, a map of the whole block over them, unmapping the pieces in reverse, and the
// whole block mapped once they're gone.
//
// On a PS Vita TV on firmware 3.65 all 4096 maps of 4 KiB in the 16 MiB block work, and so do their
// unmaps. The 32 pieces of 1 MiB map, a map of the whole block over them gives 0x805B0017 (DRIVER),
// and once they're unmapped the whole block maps.
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

#define PAGES 4096

static void display_callback(const void *data) {
    (void)data;
}

static int initialize(void) {
    SceGxmInitializeParams init = { 0 };
    init.displayQueueMaxPendingCount = 2;
    init.displayQueueCallback = display_callback;
    init.displayQueueCallbackDataSize = 4;
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    return sceGxmInitialize(&init);
}

static char *alloc(SceUID *uid, SceSize size) {
    *uid = sceKernelAllocMemBlock("gxm many", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, NULL);
    void *base = NULL;
    if (*uid >= 0)
        sceKernelGetMemBlockBase(*uid, &base);
    return base;
}

static void pages(void) {
    SceUID block;
    char *const base = alloc(&block, PAGES * 4 * KIB);
    PRINT("allocate a 16 MiB block", test_created(block));
    if (block < 0)
        return;

    int count = 0, error = 0;
    while (count < PAGES) {
        error = sceGxmMapMemory(base + count * 4 * KIB, 4 * KIB, RW);
        if (error != 0)
            break;
        count++;
    }
    PRINT("4 KiB maps: at least 64 work", count >= 64);
    PRINT("4 KiB maps: at least 256 work", count >= 256);
    PRINT("4 KiB maps: at least 1024 work", count >= 1024);
    PRINT("4 KiB maps: all 4096 work", count == PAGES);
    PRINT("4 KiB maps: the error of the one that fails", error);

    int failed = 0;
    for (int i = 0; i < count; i++)
        failed += sceGxmUnmapMemory(base + i * 4 * KIB) != 0;
    PRINT("4 KiB maps: unmaps that fail", failed);
    sceKernelFreeMemBlock(block);
}

static void pieces(void) {
    SceUID block;
    char *const base = alloc(&block, 32 * MIB);
    PRINT("allocate a 32 MiB block", test_created(block));
    if (block < 0)
        return;

    // One bit for each piece that maps
    SceUInt32 mapped = 0;
    int failed = 0;
    for (int i = 0; i < 32; i++) {
        const int error = sceGxmMapMemory(base + i * MIB, MIB, RW);
        if (error == 0)
            mapped |= 1u << i;
        else if (failed++ == 0)
            PRINTF(error, "1 MiB pieces: the first map that fails, piece %d", i);
    }
    PRINT("1 MiB pieces: maps that fail", failed);

    const int whole = sceGxmMapMemory(base, 32 * MIB, RW);
    PRINT("1 MiB pieces: the whole block over them", whole);
    if (whole == 0)
        sceGxmUnmapMemory(base);

    failed = 0;
    for (int i = 31; i >= 0; i--)
        if (mapped & (1u << i))
            failed += sceGxmUnmapMemory(base + i * MIB) != 0;
    PRINT("1 MiB pieces: unmaps in reverse that fail", failed);

    const int after = sceGxmMapMemory(base, 32 * MIB, RW);
    PRINT("1 MiB pieces: the whole block once they're unmapped", after);
    if (after == 0)
        PRINT("1 MiB pieces: unmap the whole block", sceGxmUnmapMemory(base));
    sceKernelFreeMemBlock(block);
}

int main(void) {
    PRINT("initialize libgxm", initialize());
    pages();
    pieces();
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
