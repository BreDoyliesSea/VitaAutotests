// What sceGxmMapMemory and sceGxmUnmapMemory take, and how a mapping relates to the memory blocks
// under it. No GPU work runs, so a mapping is only made and undone. Every map that works is
// unmapped again unless the case says otherwise. The cases:
// - a map before sceGxmInitialize
// - a whole 1 MiB USER_RW_UNCACHE block with each attribute value: 0, READ, WRITE, READ and WRITE,
//   4 and 0xFFFFFFFF
// - a base that isn't 4 KiB aligned, a size that isn't a multiple of 4 KiB, and size 0
// - a whole USER_CDRAM_RW block, a whole cached USER_RW block, and a read only block with READ,
//   WRITE, and both
// - a range that runs past the end of a block, and the main thread's stack. gxm_map_no_block maps a
//   range in no block, since a wrong implementation may crash on it, and memblock_neighbors one
//   over two blocks next to each other, since blocks don't always go in a row.
// - with the first half of a block mapped, that range again, a range inside it, and one over its
//   end that stays in the block. Then two halves of a block mapped apart.
// - unmapping an address that isn't mapped, the middle of a mapping, and a mapping twice
// - freeing a mapped block, then unmapping it, with what its UID and address find after each
// - sceGxmMapVertexUsseMemory and sceGxmMapFragmentUsseMemory with 4 KiB, a size over 8 MiB and an
//   unaligned base, and whether two mappings get different offsets
// - sceGxmTerminate with a mapping left, then a map after it
// - last, with libgxm initialized again, a NULL base and NULL USSE offsets
//
// On a PS Vita TV on firmware 3.65 a map before sceGxmInitialize or after sceGxmTerminate fails
// with 0x805B0000 (SCE_GXM_ERROR_UNINITIALIZED). Only READ, WRITE and both are valid attributes. A
// base or size that isn't 4 KiB aligned, a range that isn't all inside one memory block, a range
// that overlaps a mapping, and WRITE on a read only block, alone or with READ, fail with 0x805B0017
// (SCE_GXM_ERROR_DRIVER), as does an unmap of anything but the base of a mapping. A NULL pointer
// gives 0x805B0004 (SCE_GXM_ERROR_INVALID_POINTER). Cached, uncached and CDRAM blocks and the stack
// can all be mapped, size 0 maps, and two halves of a block can be mapped apart. Freeing a mapped
// block fails with 0x80024703 (SCE_KERNEL_ERROR_BLOCK_IN_USE), yet its UID is gone at once: a
// lookup by its UID or by its address gives 0x80024501 (SCE_KERNEL_ERROR_INVALID_UID), before and
// after the unmap, and so does a second free. USSE maps take up to 8 MiB, more gives 0x805B0003
// (SCE_GXM_ERROR_INVALID_VALUE), each gets its own offset, and they can overlap a sceGxmMapMemory
// mapping. sceGxmTerminate works with a mapping left.
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

typedef struct Block {
    SceUID uid;
    char *base;
    SceSize size;
} Block;

static Block alloc(SceUInt32 type, SceSize size) {
    Block block = { .uid = sceKernelAllocMemBlock("gxm", type, size, NULL), .size = size };
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

// Maps, prints the result and unmaps a mapping that worked, printing that too
static void map_once(const char *label, void *base, SceSize size, SceUInt32 attribs) {
    const int mapped = sceGxmMapMemory(base, size, attribs);
    PRINTF(mapped, "%s", label);
    if (mapped == 0)
        PRINTF(sceGxmUnmapMemory(base), "%s: unmap it", label);
}

static void ranges(void) {
    const Block uncached = alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, MIB);
    static const SceUInt32 attribs[] = { 0, SCE_GXM_MEMORY_ATTRIB_READ, SCE_GXM_MEMORY_ATTRIB_WRITE, RW, 4, 0xFFFFFFFF };
    for (unsigned int i = 0; i < sizeof(attribs) / sizeof(attribs[0]); i++) {
        char label[64];
        sceClibSnprintf(label, sizeof(label), "a whole uncached block, attributes 0x%X", attribs[i]);
        map_once(label, uncached.base, uncached.size, attribs[i]);
    }
    map_once("a base 2 KiB into the block", uncached.base + 2 * KIB, 64 * KIB, RW);
    map_once("a size of 6 KiB", uncached.base, 6 * KIB, RW);
    map_once("a size of 0", uncached.base, 0, RW);
    map_once("64 KiB that ends 32 KiB past the block", uncached.base + uncached.size - 32 * KIB, 64 * KIB, RW);

    const Block cdram = alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB);
    map_once("a whole CDRAM block", cdram.base, cdram.size, RW);
    const Block cached = alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB);
    map_once("a whole cached block", cached.base, cached.size, RW);
    const Block read_only = alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R, 64 * KIB);
    map_once("a read only block with READ", read_only.base, read_only.size, SCE_GXM_MEMORY_ATTRIB_READ);
    map_once("a read only block with WRITE", read_only.base, read_only.size, SCE_GXM_MEMORY_ATTRIB_WRITE);
    map_once("a read only block with READ and WRITE", read_only.base, read_only.size, RW);
    sceKernelFreeMemBlock(read_only.uid);
    sceKernelFreeMemBlock(cached.uid);
    sceKernelFreeMemBlock(cdram.uid);

    volatile int on_stack = 0;
    char *stack_page = (char *)((SceUIntPtr)&on_stack & ~(SceUIntPtr)(4 * KIB - 1));
    map_once("the main thread's stack page", stack_page, 4 * KIB, RW);
    (void)on_stack;

    sceKernelFreeMemBlock(uncached.uid);
}

// Prints what the UID and the address of a block still find
static void lookup(const char *when, const Block *block) {
    void *base = NULL;
    PRINTF(sceKernelGetMemBlockBase(block->uid, &base), "%s: the base by its UID", when);
    const SceUID found = sceKernelFindMemBlockByAddr(block->base, 0);
    PRINTF(found == block->uid ? 0 : found, "%s: find by its address", when);
}

static void overlaps(void) {
    const Block block = alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, MIB);
    // The mapping is the block's first half, so every overlapping range stays inside the block
    const SceSize half = block.size / 2;
    PRINT("map the first half of the block", sceGxmMapMemory(block.base, half, RW));
    PRINT("map it again", sceGxmMapMemory(block.base, half, RW));
    PRINT("map 64 KiB inside it", sceGxmMapMemory(block.base + 64 * KIB, 64 * KIB, RW));
    PRINT("map 64 KiB over its end", sceGxmMapMemory(block.base + half - 32 * KIB, 64 * KIB, RW));
    PRINT("unmap the middle of it", sceGxmUnmapMemory(block.base + 64 * KIB));
    PRINT("unmap it", sceGxmUnmapMemory(block.base));
    PRINT("unmap it again", sceGxmUnmapMemory(block.base));
    PRINT("unmap an address that isn't mapped", sceGxmUnmapMemory(block.base + 4 * KIB));

    PRINT("map the first half", sceGxmMapMemory(block.base, block.size / 2, RW));
    PRINT("map the second half", sceGxmMapMemory(block.base + block.size / 2, block.size / 2, RW));
    PRINT("unmap the second half", sceGxmUnmapMemory(block.base + block.size / 2));
    PRINT("unmap the first half", sceGxmUnmapMemory(block.base));

    PRINT("map it before freeing it", sceGxmMapMemory(block.base, block.size, RW));
    PRINT("free the mapped block", sceKernelFreeMemBlock(block.uid));
    lookup("after the free", &block);
    PRINT("unmap it after the free", sceGxmUnmapMemory(block.base));
    lookup("after the unmap", &block);
    PRINT("free it after the unmap", sceKernelFreeMemBlock(block.uid));
}

static void usse(void) {
    const Block block = alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 16 * MIB);
    SceUInt32 offset = 0, offset2 = 0;
    PRINT("vertex USSE: 4 KiB", sceGxmMapVertexUsseMemory(block.base, 4 * KIB, &offset));
    PRINT("vertex USSE: another 4 KiB", sceGxmMapVertexUsseMemory(block.base + 64 * KIB, 4 * KIB, &offset2));
    PRINT("vertex USSE: the offsets differ", offset != offset2);
    PRINT("vertex USSE: unmap the first", sceGxmUnmapVertexUsseMemory(block.base));
    PRINT("vertex USSE: unmap the second", sceGxmUnmapVertexUsseMemory(block.base + 64 * KIB));
    PRINT("vertex USSE: unmap again", sceGxmUnmapVertexUsseMemory(block.base));
    const int big = sceGxmMapVertexUsseMemory(block.base, 8 * MIB + 4 * KIB, &offset);
    PRINT("vertex USSE: 8 MiB and 4 KiB", big);
    if (big == 0)
        sceGxmUnmapVertexUsseMemory(block.base);
    const int eight = sceGxmMapVertexUsseMemory(block.base, 8 * MIB, &offset);
    PRINT("vertex USSE: 8 MiB", eight);
    if (eight == 0)
        sceGxmUnmapVertexUsseMemory(block.base);
    const int unaligned = sceGxmMapVertexUsseMemory(block.base + 2 * KIB, 4 * KIB, &offset);
    PRINT("vertex USSE: an unaligned base", unaligned);
    if (unaligned == 0)
        sceGxmUnmapVertexUsseMemory(block.base + 2 * KIB);

    // Both start the same, so only the maps can make them differ
    offset = offset2 = 0;
    PRINT("fragment USSE: 4 KiB", sceGxmMapFragmentUsseMemory(block.base, 4 * KIB, &offset));
    PRINT("fragment USSE: another 4 KiB", sceGxmMapFragmentUsseMemory(block.base + 64 * KIB, 4 * KIB, &offset2));
    PRINT("fragment USSE: the offsets differ", offset != offset2);
    PRINT("fragment USSE: unmap the first", sceGxmUnmapFragmentUsseMemory(block.base));
    PRINT("fragment USSE: unmap the second", sceGxmUnmapFragmentUsseMemory(block.base + 64 * KIB));
    const int fbig = sceGxmMapFragmentUsseMemory(block.base, 8 * MIB + 4 * KIB, &offset);
    PRINT("fragment USSE: 8 MiB and 4 KiB", fbig);
    if (fbig == 0)
        sceGxmUnmapFragmentUsseMemory(block.base);

    PRINT("map memory then vertex USSE over it", sceGxmMapMemory(block.base, MIB, RW));
    const int both = sceGxmMapVertexUsseMemory(block.base, 4 * KIB, &offset);
    PRINT("map memory then vertex USSE over it: the USSE map", both);
    if (both == 0)
        sceGxmUnmapVertexUsseMemory(block.base);
    sceGxmUnmapMemory(block.base);
    sceKernelFreeMemBlock(block.uid);
}

int main(void) {
    const Block early = alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 64 * KIB);
    PRINT("map before initializing", sceGxmMapMemory(early.base, early.size, RW));

    PRINT("initialize libgxm", initialize());

    ranges();
    overlaps();
    usse();

    PRINT("map a block to leave mapped", sceGxmMapMemory(early.base, early.size, RW));
    PRINT("terminate libgxm with it mapped", sceGxmTerminate());
    PRINT("map after terminating", sceGxmMapMemory(early.base, early.size, RW));

    // NULL pointers go last, so an implementation that crashes on them loses nothing else
    PRINT("initialize libgxm again", initialize());
    PRINT("a NULL base", sceGxmMapMemory(NULL, 64 * KIB, RW));
    PRINT("vertex USSE: a NULL offset", sceGxmMapVertexUsseMemory(early.base, 4 * KIB, NULL));
    PRINT("fragment USSE: a NULL offset", sceGxmMapFragmentUsseMemory(early.base, 4 * KIB, NULL));
    PRINT("terminate libgxm again", sceGxmTerminate());
    sceKernelFreeMemBlock(early.uid);
    test_exit();
    return 0;
}
