// Opening a memory block with sceKernelOpenMemBlock, past what memblock_opt_effects covers: which
// flag bits it takes, and for each opened block the access, memory type, type and mapped size the
// info call reports. Then for a block opened with 0x30: its region, which UID the find call gives
// for the original's base and for its own, a write through it showing in the original, the free
// sizes it takes, opening the block a second time, closing and freeing the opened blocks, a CDRAM
// block, closing a block that was allocated instead of opened, and freeing the original while it's
// open.
//
// a PS Vita TV on firmware 3.65 only flags 0x10 and 0x30 work, every other bit fails with
// 0x80020005 (SCE_KERNEL_ERROR_INVALID_ARGUMENT). 0x10 opens a USER_RW block read only, as type
// 0x0C20D040 with access 0x4, and 0x30 read and write with the original's type. An opened block
// takes no memory, maps the same memory at another address in the same region, and each opening
// gives another UID and address. The usual free and sceKernelCloseMemBlock both end an opened
// block, and close ends an allocated block too. A CDRAM block fails to open with 0x80024804.
// Reading through an opened block after its original is freed faults, so the test leaves that out.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000

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

static volatile SceUInt32 *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

static SceKernelMemBlockInfo info_of(SceUID block) {
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    const int error = sceKernelGetMemBlockInfoByAddr((void *)base_of(block), &info);
    if (error < 0)
        PRINT("get the info", error);
    return info;
}

// Opens the block by name with the flags, prints what the info call reports about it and closes it
static void try_flags(const char *name, SceUInt32 flags, SceSize size) {
    const SceUID opened = sceKernelOpenMemBlock(name, flags);
    PRINTF(test_created(opened), "flags 0x%X", (unsigned)flags);
    if (opened < 0)
        return;
    const SceKernelMemBlockInfo info = info_of(opened);
    PRINTF(info.access, "flags 0x%X: its access", (unsigned)flags);
    PRINTF(info.memoryType, "flags 0x%X: its memory type", (unsigned)flags);
    PRINTF(info.type, "flags 0x%X: its type", (unsigned)flags);
    PRINTF(info.mappedSize == size, "flags 0x%X: its mapped size is the original's", (unsigned)flags);
    PRINTF(sceKernelCloseMemBlock(opened), "flags 0x%X: close it", (unsigned)flags);
}

int main(void) {
    const SceUID original = alloc_openable("openable", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB);
    PRINT("allocate an openable USER_RW block", test_created(original));
    if (original < 0) {
        test_exit();
        return 0;
    }
    volatile SceUInt32 *mine = base_of(original);

    // Flag bits
    static const SceUInt32 flags[] = { 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x30, 0x40, 0x80, 0x100, 0x110, 0x1000, 0x10000, 0x80000000 };
    for (unsigned int i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
        try_flags("openable", flags[i], 64 * KIB);

    // A block opened with 0x30
    SceKernelFreeMemorySizeInfo before = { .size = sizeof(before) }, after = { .size = sizeof(after) };
    sceKernelGetFreeMemorySize(&before);
    const SceUID opened = sceKernelOpenMemBlock("openable", 0x30);
    sceKernelGetFreeMemorySize(&after);
    PRINT("open it with 0x30", test_created(opened));
    if (opened >= 0) {
        volatile SceUInt32 *theirs = base_of(opened);
        PRINT("the main memory opening it takes", before.size_user - after.size_user);
        PRINT("its region", (SceUIntPtr)theirs >> 28);
        PRINT("find the original's base", sceKernelFindMemBlockByAddr((void *)mine, 0) == original);
        PRINT("find the opened block's base", sceKernelFindMemBlockByAddr((void *)theirs, 0) == opened);
        if (info_of(opened).access & 0x2) {
            theirs[1] = 0x5A5A1234;
            PRINT("a write through it shows in the original", mine[1] == 0x5A5A1234);
        }

        const SceUID again = sceKernelOpenMemBlock("openable", 0x30);
        PRINT("open it a second time", test_created(again));
        if (again >= 0) {
            PRINT("the second one gives another UID", again != opened);
            PRINT("the second one maps at another address", base_of(again) != theirs);
            PRINT("close the second one", sceKernelCloseMemBlock(again));
            PRINT("close the second one again", sceKernelCloseMemBlock(again));
        }
        PRINT("free the opened block with the usual free", sceKernelFreeMemBlock(opened));
        PRINT("close the opened block after that", sceKernelCloseMemBlock(opened));
    }

    // A CDRAM block
    const SceUID cdram = alloc_openable("openable cdram", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB);
    PRINT("allocate an openable CDRAM block", test_created(cdram));
    if (cdram >= 0) {
        const SceUID cdram_opened = sceKernelOpenMemBlock("openable cdram", 0x30);
        PRINT("open it with 0x30", test_created(cdram_opened));
        if (cdram_opened >= 0) {
            const SceKernelMemBlockInfo info = info_of(cdram_opened);
            PRINT("its memory type", info.memoryType);
            PRINT("its type is the original's", info.type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW);
            PRINT("its region", (SceUIntPtr)base_of(cdram_opened) >> 28);
            PRINT("close it", sceKernelCloseMemBlock(cdram_opened));
        }
        sceKernelFreeMemBlock(cdram);
    }

    // Closing a block that was allocated, not opened
    const SceUID closed = alloc_openable("closed", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB);
    PRINT("allocate another openable block", test_created(closed));
    if (closed >= 0) {
        volatile SceUInt32 *closed_base = base_of(closed);
        PRINT("close it", sceKernelCloseMemBlock(closed));
        PRINT("find it after the close", sceKernelFindMemBlockByAddr((void *)closed_base, 0) == closed);
        PRINT("free it after the close", sceKernelFreeMemBlock(closed));
    }

    // The original freed while an opened block lives. Nothing reads through the opened block after
    // that, since that faults on a Vita.
    const SceUID last = sceKernelOpenMemBlock("openable", 0x30);
    PRINT("open the block once more", test_created(last));
    volatile SceUInt32 *last_base = last >= 0 ? base_of(last) : NULL;
    PRINT("free the original while it's open", sceKernelFreeMemBlock(original));
    PRINT("find the original's base after that", sceKernelFindMemBlockByAddr((void *)mine, 0) == original);
    if (last >= 0) {
        PRINT("find the opened block's base after that", sceKernelFindMemBlockByAddr((void *)last_base, 0) == last);
        PRINT("close the opened block after that", sceKernelCloseMemBlock(last));
    }

    test_exit();
    return 0;
}
