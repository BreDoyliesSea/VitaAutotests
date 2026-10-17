// How sceGxmMapMemory mappings of size 0 sit with others, and unmaps given a base 1 byte or 4 KiB
// less 1 byte past a mapping's base. The cases, each on a fresh 1 MiB USER_RW_UNCACHE block:
// - unmapping 1 byte and 4 KiB less 1 byte past a 64 KiB mapping's base, then its base
// - size 0 at a base 2 KiB into the block, which isn't 4 KiB aligned
// - size 0 twice at one base, and then 64 KiB mapped at that base
// - 64 KiB at a base, then size 0 at that base, 16 KiB into the mapping and at its end
// After each case every base it mapped is unmapped until that fails, printing each unmap, so
// nothing it mapped is left. Then, since a wrong implementation may crash on them, freeing a block
// with size 0 mapped at its base, what its UID finds, the unmap and freeing it again, then size 0
// where a block was freed, in no block, and last a NULL base for sceGxmUnmapMemory.
//
// On a PS Vita TV on firmware 3.65 an unmap takes only a mapping's exact base, 1 byte or 4 KiB less
// 1 byte past it gives 0x805B0017 (DRIVER). Size 0 maps at a 4 KiB aligned base in a block, even at
// a mapping's base or end, but not 16 KiB into a mapping or at an unaligned base. Each size 0 map
// is a mapping of its own: two of them and 64 KiB at one base take three unmaps of that base. A
// block with size 0 mapped at its base can't be freed (0x80024703, BLOCK_IN_USE), its UID is gone
// at once, and the unmap still works. Size 0 in no block gives 0x805B0017, and unmapping NULL gives
// 0x805B0004 (INVALID_POINTER).
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

typedef struct Block {
    SceUID uid;
    char *base;
} Block;

static Block alloc(void) {
    Block block = { .uid = sceKernelAllocMemBlock("gxm size zero",
                                                  SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, MIB,
                                                  NULL) };
    void *base = NULL;
    if (block.uid >= 0)
        sceKernelGetMemBlockBase(block.uid, &base);
    block.base = base;
    return block;
}

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

// Unmaps base until that fails, at most 4 times, printing each unmap
static void unmap_all(const char *label, void *base) {
    for (int i = 0; i < 4; i++) {
        const int unmapped = sceGxmUnmapMemory(base);
        PRINTF(unmapped, "%s: unmap it, try %d", label, i + 1);
        if (unmapped != 0)
            break;
    }
}

static void unaligned_unmaps(void) {
    const Block block = alloc();
    PRINT("64 KiB to unmap", sceGxmMapMemory(block.base, 64 * KIB, RW));
    PRINT("unmap 1 byte past its base", sceGxmUnmapMemory(block.base + 1));
    PRINT("unmap 4 KiB less 1 byte past its base", sceGxmUnmapMemory(block.base + 4 * KIB - 1));
    unmap_all("64 KiB to unmap", block.base);
    sceKernelFreeMemBlock(block.uid);
}

static void zero_cases(void) {
    Block block = alloc();
    PRINT("size 0 at an unaligned base", sceGxmMapMemory(block.base + 2 * KIB, 0, RW));
    unmap_all("size 0 at an unaligned base", block.base + 2 * KIB);
    sceKernelFreeMemBlock(block.uid);

    block = alloc();
    PRINT("size 0 twice: the first", sceGxmMapMemory(block.base, 0, RW));
    PRINT("size 0 twice: the second", sceGxmMapMemory(block.base, 0, RW));
    PRINT("size 0 twice: then 64 KiB at the base", sceGxmMapMemory(block.base, 64 * KIB, RW));
    unmap_all("size 0 twice", block.base);
    sceKernelFreeMemBlock(block.uid);

    block = alloc();
    PRINT("64 KiB, then size 0: the 64 KiB", sceGxmMapMemory(block.base, 64 * KIB, RW));
    PRINT("64 KiB, then size 0: at its base", sceGxmMapMemory(block.base, 0, RW));
    PRINT("64 KiB, then size 0: 16 KiB into it", sceGxmMapMemory(block.base + 16 * KIB, 0, RW));
    PRINT("64 KiB, then size 0: at its end", sceGxmMapMemory(block.base + 64 * KIB, 0, RW));
    unmap_all("64 KiB, then size 0: at its end", block.base + 64 * KIB);
    unmap_all("64 KiB, then size 0: 16 KiB into it", block.base + 16 * KIB);
    unmap_all("64 KiB, then size 0: the base", block.base);
    sceKernelFreeMemBlock(block.uid);
}

int main(void) {
    PRINT("initialize libgxm", initialize());
    unaligned_unmaps();
    zero_cases();

    // Last, since a wrong implementation may crash on a range in no block or a NULL base
    const Block held = alloc();
    PRINT("size 0, then a free: size 0 at the base", sceGxmMapMemory(held.base, 0, RW));
    PRINT("size 0, then a free: free the block", sceKernelFreeMemBlock(held.uid));
    void *base = NULL;
    PRINT("size 0, then a free: the base by its UID", sceKernelGetMemBlockBase(held.uid, &base));
    unmap_all("size 0, then a free", held.base);
    PRINT("size 0, then a free: free it after the unmap", sceKernelFreeMemBlock(held.uid));

    const Block gone = alloc();
    sceKernelFreeMemBlock(gone.uid);
    const int nowhere = sceGxmMapMemory(gone.base, 0, RW);
    PRINT("size 0 in no block", nowhere);
    if (nowhere == 0)
        unmap_all("size 0 in no block", gone.base);
    PRINT("unmap a NULL base", sceGxmUnmapMemory(NULL));
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
