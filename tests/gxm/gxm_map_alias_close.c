// Ending a block opened with sceKernelOpenMemBlock while sceGxmMapMemory has it mapped. First
// sceKernelCloseMemBlock on a mapped block opened with 0x30: the close, what its UID and its
// address find, mapping its range again, mapping its original, which holds the same pages, the
// unmap, the lookups after it, and closing it again. Then the same with sceKernelFreeMemBlock
// instead of the close. That goes last, since on an implementation where the opened block is its
// original the free ends the mapped original, and the unmap may crash. Each case frees its
// original at the end.
//
// On a PS Vita TV on firmware 3.65 closing a mapped opened block and freeing it both give
// 0x80024703 (BLOCK_IN_USE), and its UID is gone at once: its base by UID and its address give
// 0x80024501 (INVALID_UID), before and after the unmap, while the original still finds itself. Its
// range stays taken, so mapping it again gives 0x805B0017, but the original, which holds the same
// pages at another address, maps. The unmap works, and ending the opened block again gives
// 0x80024501.
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

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

static char *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

// Prints what the UIDs and addresses of the opened block and its original find
static void lookup(const char *when, SceUID opened, char *opened_base, SceUID original,
                   char *original_base) {
    void *base = NULL;
    PRINTF(sceKernelGetMemBlockBase(opened, &base), "%s: the opened block's base by its UID", when);
    SceUID found = sceKernelFindMemBlockByAddr(opened_base, 0);
    PRINTF(found == opened ? 0 : found, "%s: find the opened block by its address", when);
    found = sceKernelFindMemBlockByAddr(original_base, 0);
    PRINTF(found == original ? 0 : found, "%s: find the original by its address", when);
}

// Maps a block opened from a new original, ends it with end while it's mapped, and checks the rest
static void end_mapped(const char *how, const char *name, int (*end)(SceUID)) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID original = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MIB,
                                                   (SceKernelAllocMemBlockOpt *)&opt);
    PRINTF(test_created(original), "%s: allocate an openable block", how);
    if (original < 0)
        return;
    char *const original_base = base_of(original);
    const SceUID opened = sceKernelOpenMemBlock(name, 0x30);
    PRINTF(test_created(opened), "%s: open it with 0x30", how);
    if (opened < 0) {
        sceKernelFreeMemBlock(original);
        return;
    }
    char *const opened_base = base_of(opened);

    const int mapped = sceGxmMapMemory(opened_base, MIB, RW);
    PRINTF(mapped, "%s: map the opened block", how);
    PRINTF(end(opened), "%s: end the mapped opened block", how);
    lookup(how, opened, opened_base, original, original_base);

    const int again = sceGxmMapMemory(opened_base, MIB, RW);
    PRINTF(again, "%s: map the opened block's range again", how);
    if (again == 0)
        PRINTF(sceGxmUnmapMemory(opened_base), "%s: unmap that", how);
    const int whole = sceGxmMapMemory(original_base, MIB, RW);
    PRINTF(whole, "%s: map the original", how);
    if (whole == 0)
        PRINTF(sceGxmUnmapMemory(original_base), "%s: unmap the original", how);

    if (mapped == 0)
        PRINTF(sceGxmUnmapMemory(opened_base), "%s: unmap the opened block", how);
    char when[64];
    sceClibSnprintf(when, sizeof(when), "%s, after the unmap", how);
    lookup(when, opened, opened_base, original, original_base);
    PRINTF(end(opened), "%s: end the opened block again", how);
    PRINTF(sceKernelFreeMemBlock(original), "%s: free the original", how);
}

int main(void) {
    PRINT("initialize libgxm", initialize());
    end_mapped("close", "gxm alias close", sceKernelCloseMemBlock);
    // Last, since this free may end the mapped original
    end_mapped("free", "gxm alias free", sceKernelFreeMemBlock);
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
