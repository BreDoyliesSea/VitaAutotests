// The address space that blocks for code from sceKernelAllocMemBlockForVM share: whether a 16 MiB
// block fits, whether sceKernelFreeMemBlockForVM gives its space back, how many 1 MiB blocks fit at
// once, and whether sceKernelFreeMemBlock gives back the space and the main memory of a block. Each
// step depends on the space the ones before it left, so their order is part of the test.
//
// On a PS Vita TV on firmware 3.65 blocks for code share 16 MiB, and sceKernelFreeMemBlockForVM
// gives a block's space back. sceKernelFreeMemBlock gives back its main memory but not its space,
// so after it no block for code fits, failing with 0x80024B0B (SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW).
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define MIB 0x100000
#define MAX_BLOCKS 20

// vitasdk has a stub for it but no prototype
int sceKernelFreeMemBlockForVM(SceUID uid);

static void get_free_sizes(SceKernelFreeMemorySizeInfo *info) {
    info->size = sizeof(*info);
    const int error = sceKernelGetFreeMemorySize(info);
    if (error < 0)
        PRINT("get the free sizes", error);
}

int main(void) {
    SceUID block = sceKernelAllocMemBlockForVM("vm", 16 * MIB);
    PRINT("allocate 16 MiB", test_created(block));
    if (block >= 0)
        PRINT("free it with the call for code", sceKernelFreeMemBlockForVM(block));

    block = sceKernelAllocMemBlockForVM("vm", 16 * MIB);
    PRINT("allocate 16 MiB again", test_created(block));
    if (block >= 0)
        PRINT("free it with the call for code", sceKernelFreeMemBlockForVM(block));

    SceUID blocks[MAX_BLOCKS];
    int count = 0;
    SceUID error = 0;
    while (count < MAX_BLOCKS) {
        const SceUID uid = sceKernelAllocMemBlockForVM("vm", MIB);
        if (uid < 0) {
            error = uid;
            break;
        }
        blocks[count++] = uid;
    }
    PRINT("1 MiB blocks that fit at once", count);
    PRINT("the error for the next one", error);
    for (int i = 0; i < count; i++)
        sceKernelFreeMemBlockForVM(blocks[i]);

    SceKernelFreeMemorySizeInfo before = { 0 }, after = { 0 };
    get_free_sizes(&before);
    block = sceKernelAllocMemBlockForVM("vm", 16 * MIB);
    PRINT("allocate 16 MiB after freeing them", test_created(block));
    if (block >= 0) {
        PRINT("free it with the usual call", sceKernelFreeMemBlock(block));
        get_free_sizes(&after);
        PRINT("the usual free gives back its main memory", before.size_user == after.size_user);
    }
    PRINT("allocate 1 MiB after that", test_created(sceKernelAllocMemBlockForVM("vm", MIB)));

    test_exit();
    return 0;
}
