// The calls given a range over two memory blocks next to each other. A Vita doesn't promise that
// any two blocks are, so this looks for a pair among 32 blocks of 64 KiB USER_RW_UNCACHE, and ends
// with test_skip() if there's none. They're apart from memblock_placement and gxm_map_memory, so
// those don't depend on where blocks go. The cases, for the range over both blocks:
// - sceKernelFindMemBlockByAddr, and with 8 KiB over the boundary
// - sceKernelGetMemBlockInfoByRange
// - sceGxmMapMemory, after sceGxmInitialize
//
// On a PS Vita TV on firmware 3.65 two of the blocks were next to each other in every run. The find
// fails with 0x80020001 (SCE_KERNEL_ERROR_ERROR), the info call with 0x80024B09
// (SCE_KERNEL_ERROR_MEMBLOCK_RANGE_ERROR), and the map with 0x805B0017 (SCE_GXM_ERROR_DRIVER).
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define BLOCKS 32

typedef struct Block {
    SceUID uid;
    char *base;
} Block;

static void display_callback(const void *data) {
    (void)data;
}

int main(void) {
    static Block blocks[BLOCKS];
    int made = 1;
    for (int i = 0; i < BLOCKS; i++) {
        blocks[i].uid = sceKernelAllocMemBlock("neighbor", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 64 * KIB, NULL);
        void *base = NULL;
        made &= blocks[i].uid >= 0 && sceKernelGetMemBlockBase(blocks[i].uid, &base) == 0 && base != NULL;
        blocks[i].base = base;
    }
    // A block that isn't made fails the test, since only a layout without neighbors is a skip
    if (!made) {
        PRINT("the blocks are made", 0);
        test_exit();
        return 0;
    }
    const Block *a = NULL, *b = NULL;
    for (int i = 0; i < BLOCKS && !a; i++)
        for (int j = 0; j < BLOCKS && !a; j++)
            if (blocks[j].base == blocks[i].base + 64 * KIB) {
                a = &blocks[i];
                b = &blocks[j];
            }
    if (!a) {
        for (int i = 0; i < BLOCKS; i++)
            sceKernelFreeMemBlock(blocks[i].uid);
        test_skip("no two blocks are next to each other");
        return 0;
    }
    PRINT("two blocks are next to each other", 1);
    const SceUID across = sceKernelFindMemBlockByAddr(a->base, 128 * KIB);
    PRINT("find the range over both", across == a->uid || across == b->uid ? 0 : across);
    PRINT("find the range over both: the first", across == a->uid);
    PRINT("find the range over both: the second", across == b->uid);
    const SceUID straddle = sceKernelFindMemBlockByAddr(a->base + 60 * KIB, 8 * KIB);
    PRINT("find 8 KiB over the boundary", straddle == a->uid || straddle == b->uid ? 0 : straddle);
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    PRINT("info for the range over both", sceKernelGetMemBlockInfoByRange(a->base, 128 * KIB, &info));

    SceGxmInitializeParams init = { 0 };
    init.displayQueueMaxPendingCount = 2;
    init.displayQueueCallback = display_callback;
    init.displayQueueCallbackDataSize = 4;
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    PRINT("initialize libgxm", sceGxmInitialize(&init));
    const int mapped = sceGxmMapMemory(a->base, 128 * KIB, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
    PRINT("map the range over both", mapped);
    if (mapped == 0)
        PRINT("unmap it", sceGxmUnmapMemory(a->base));
    PRINT("terminate libgxm", sceGxmTerminate());
    for (int i = 0; i < BLOCKS; i++)
        sceKernelFreeMemBlock(blocks[i].uid);
    test_exit();
    return 0;
}
