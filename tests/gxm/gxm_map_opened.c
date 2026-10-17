// sceGxmMapMemory and the USSE maps on blocks opened with sceKernelOpenMemBlock, which map the
// pages of their original at an address of their own. No GPU work runs. The cases:
// - a whole block opened with 0x30 from a USER_RW block, with READ, WRITE, and both
// - a whole block opened with 0x10, which is read only, with READ, WRITE, and both
// - the original and its 0x30 block mapped at once, in both orders, then the original and its 0x10
//   block
// - with only the original mapped, unmapping the opened block's base, and the other way around
// - a whole block opened with 0x30 from a USER_RW_UNCACHE block and from a USER_CDIALOG_RW block
// - a vertex USSE map of 4 KiB of the 0x30 block and a fragment one of the 0x10 block
// Every map that works is unmapped again, and every opened block is closed with
// sceKernelCloseMemBlock before its original is freed.
//
// On a PS Vita TV on firmware 3.65 a block opened with 0x30 maps like any block, with READ, WRITE
// or both, and so does its original at the same time, in either order, since the two are at
// different addresses. Each mapping is unmapped only by its own base, the other block's gives
// 0x805B0017 (DRIVER). The block opened with 0x10 maps only with READ, WRITE gives 0x805B0017. Both
// kinds of USSE map take an opened block, and sceGxmMapMemory takes opened USER_RW_UNCACHE and
// USER_CDIALOG_RW blocks.
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000
#define ENABLE_OPEN 0x4000

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

// The option struct games pass, without the fields vitasdk adds after strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static SceUID alloc_openable(const char *name, SceUInt32 type, SceSize size) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    return sceKernelAllocMemBlock(name, type, size, (SceKernelAllocMemBlockOpt *)&opt);
}

static char *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
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

// Maps the range with READ, WRITE, and both, one after another
static void map_each_attrib(const char *what, void *base, SceSize size) {
    static const struct {
        SceUInt32 attribs;
        const char *name;
    } attribs[] = {
        { SCE_GXM_MEMORY_ATTRIB_READ, "READ" },
        { SCE_GXM_MEMORY_ATTRIB_WRITE, "WRITE" },
        { RW, "READ and WRITE" },
    };
    for (unsigned int i = 0; i < sizeof(attribs) / sizeof(attribs[0]); i++) {
        char label[96];
        sceClibSnprintf(label, sizeof(label), "%s with %s", what, attribs[i].name);
        map_once(label, base, size, attribs[i].attribs);
    }
}

// Maps first and then second whole, unmapping second and then first
static void map_both(const char *label, char *first, SceUInt32 first_attribs, char *second,
                     SceUInt32 second_attribs, SceSize size) {
    const int mapped = sceGxmMapMemory(first, size, first_attribs);
    PRINTF(mapped, "%s: map the first", label);
    const int also = sceGxmMapMemory(second, size, second_attribs);
    PRINTF(also, "%s: map the second", label);
    if (also == 0)
        PRINTF(sceGxmUnmapMemory(second), "%s: unmap the second", label);
    if (mapped == 0)
        PRINTF(sceGxmUnmapMemory(first), "%s: unmap the first", label);
}

// With only mapped mapped, unmaps the base of other, then of mapped
static void unmap_other(const char *label, char *mapped, char *other, SceSize size) {
    const int map = sceGxmMapMemory(mapped, size, RW);
    PRINTF(map, "%s: map", label);
    PRINTF(sceGxmUnmapMemory(other), "%s: unmap the other one's base", label);
    if (map == 0)
        PRINTF(sceGxmUnmapMemory(mapped), "%s: unmap its own base", label);
}

static void user_rw(void) {
    const SceUID original = alloc_openable("gxm opened rw", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MIB);
    PRINT("allocate an openable USER_RW block", test_created(original));
    if (original < 0)
        return;
    char *const mine = base_of(original);
    const SceUID rw = sceKernelOpenMemBlock("gxm opened rw", 0x30);
    PRINT("open it with 0x30", test_created(rw));
    const SceUID ro = sceKernelOpenMemBlock("gxm opened rw", 0x10);
    PRINT("open it with 0x10", test_created(ro));

    if (rw >= 0) {
        char *const theirs = base_of(rw);
        map_each_attrib("the 0x30 block", theirs, MIB);
        map_both("the original, then the 0x30 block", mine, RW, theirs, RW, MIB);
        map_both("the 0x30 block, then the original", theirs, RW, mine, RW, MIB);
        unmap_other("only the original mapped", mine, theirs, MIB);
        unmap_other("only the 0x30 block mapped", theirs, mine, MIB);

        SceUInt32 offset = 0;
        const int usse = sceGxmMapVertexUsseMemory(theirs, 4 * KIB, &offset);
        PRINT("vertex USSE: 4 KiB of the 0x30 block", usse);
        if (usse == 0)
            PRINT("vertex USSE: unmap it", sceGxmUnmapVertexUsseMemory(theirs));
    }
    if (ro >= 0) {
        char *const theirs = base_of(ro);
        map_each_attrib("the 0x10 block", theirs, MIB);
        map_both("the original, then the 0x10 block with READ", mine, RW, theirs,
                 SCE_GXM_MEMORY_ATTRIB_READ, MIB);

        SceUInt32 offset = 0;
        const int usse = sceGxmMapFragmentUsseMemory(theirs, 4 * KIB, &offset);
        PRINT("fragment USSE: 4 KiB of the 0x10 block", usse);
        if (usse == 0)
            PRINT("fragment USSE: unmap it", sceGxmUnmapFragmentUsseMemory(theirs));
    }

    if (ro >= 0)
        PRINT("close the 0x10 block", sceKernelCloseMemBlock(ro));
    if (rw >= 0)
        PRINT("close the 0x30 block", sceKernelCloseMemBlock(rw));
    PRINT("free the original", sceKernelFreeMemBlock(original));
}

// Opens a whole block of the type with 0x30 and maps it with READ and WRITE
static void other_type(const char *what, const char *name, SceUInt32 type, SceSize size) {
    const SceUID original = alloc_openable(name, type, size);
    PRINTF(test_created(original), "%s: allocate it", what);
    if (original < 0)
        return;
    const SceUID opened = sceKernelOpenMemBlock(name, 0x30);
    PRINTF(test_created(opened), "%s: open it with 0x30", what);
    if (opened >= 0) {
        char label[96];
        sceClibSnprintf(label, sizeof(label), "%s: map the opened block", what);
        map_once(label, base_of(opened), size, RW);
        sceKernelCloseMemBlock(opened);
    }
    sceKernelFreeMemBlock(original);
}

int main(void) {
    PRINT("initialize libgxm", initialize());
    user_rw();
    other_type("USER_RW_UNCACHE", "gxm opened nc", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, MIB);
    other_type("USER_CDIALOG_RW", "gxm opened cdialog", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_RW,
               64 * KIB);
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
