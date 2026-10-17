// Freeing the original of a block opened with sceKernelOpenMemBlock while sceGxmMapMemory has one
// of the two mapped. First with only the opened block mapped: the free, what the UIDs and addresses
// of both find, and mapping the original's range again. Then with only the original mapped: the
// free, the lookups, and mapping the opened block, which holds the same pages. The unmaps and
// closes of what's left come last, after both frees, since an implementation that frees memory
// that's still mapped may crash on them. Then, for the second case, what its original's address
// finds and whether a block may take its name. That name ends with the system time, so it is new
// on every run, in case the original outlives the run. Nothing reads or writes the blocks.
//
// On a PS Vita TV on firmware 3.65 freeing the original while only the opened block is mapped
// works: the original's UID and address are gone (0x80024501, 0x80022007), and its range no longer
// maps, while the opened block keeps its UID, which gives its base, but its address finds nothing.
// Freeing the original while it is mapped gives 0x80024703 (BLOCK_IN_USE), drops its UID at once
// and leaves it in place: its address gives 0x80024501, and the opened block's range doesn't map.
// After the unmaps the opened blocks close, and the original mapped while freed stays: its address
// still gives 0x80024501, and its name can't be taken (0x80024E02, DUPLICATE_NAME). That block and
// its name outlive the run, until the Vita reboots, which is why the name is new on every run.
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

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

// An original block and a block opened from it with 0x30, and which of them the case mapped
typedef struct Pair {
    SceUID original, opened;
    char *original_base, *opened_base;
    int original_mapped, opened_mapped;
} Pair;

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

// Allocates an openable USER_RW block named name and opens it with 0x30. Returns whether both work.
static int make_pair(const char *how, const char *name, Pair *pair) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    pair->original = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MIB,
                                            (SceKernelAllocMemBlockOpt *)&opt);
    PRINTF(test_created(pair->original), "%s: allocate an openable block", how);
    if (pair->original < 0)
        return 0;
    pair->original_base = base_of(pair->original);
    pair->opened = sceKernelOpenMemBlock(name, 0x30);
    PRINTF(test_created(pair->opened), "%s: open it with 0x30", how);
    if (pair->opened < 0) {
        sceKernelFreeMemBlock(pair->original);
        return 0;
    }
    pair->opened_base = base_of(pair->opened);
    pair->original_mapped = pair->opened_mapped = -1;
    return 1;
}

// Prints what the UIDs and addresses of both blocks of the pair find
static void lookup(const char *how, const Pair *pair) {
    void *base = NULL;
    PRINTF(sceKernelGetMemBlockBase(pair->original, &base), "%s: the original's base by its UID",
           how);
    SceUID found = sceKernelFindMemBlockByAddr(pair->original_base, 0);
    PRINTF(found == pair->original ? 0 : found, "%s: find the original by its address", how);
    PRINTF(sceKernelGetMemBlockBase(pair->opened, &base), "%s: the opened block's base by its UID",
           how);
    found = sceKernelFindMemBlockByAddr(pair->opened_base, 0);
    PRINTF(found == pair->opened ? 0 : found, "%s: find the opened block by its address", how);
}

// Unmaps whatever the case left mapped and closes the opened block
static void clean_up(const char *how, const Pair *pair) {
    if (pair->opened_mapped == 0)
        PRINTF(sceGxmUnmapMemory(pair->opened_base), "%s: unmap the opened block", how);
    if (pair->original_mapped == 0)
        PRINTF(sceGxmUnmapMemory(pair->original_base), "%s: unmap the original", how);
    PRINTF(sceKernelCloseMemBlock(pair->opened), "%s: close the opened block", how);
    PRINTF(sceKernelFreeMemBlock(pair->original), "%s: free the original again", how);
}

int main(void) {
    PRINT("initialize libgxm", initialize());

    // Only the opened block mapped
    Pair first;
    const int have_first = make_pair("opened mapped", "gxm original a", &first);
    if (have_first) {
        first.opened_mapped = sceGxmMapMemory(first.opened_base, MIB, RW);
        PRINT("opened mapped: map the opened block", first.opened_mapped);
        PRINT("opened mapped: free the original", sceKernelFreeMemBlock(first.original));
        lookup("opened mapped", &first);
        first.original_mapped = sceGxmMapMemory(first.original_base, MIB, RW);
        PRINT("opened mapped: map the original's range", first.original_mapped);
    }

    // Only the original mapped
    char second_name[32];
    sceClibSnprintf(second_name, sizeof(second_name), "gxm b %016llX",
                    sceKernelGetSystemTimeWide());
    Pair second;
    const int have_second = make_pair("original mapped", second_name, &second);
    if (have_second) {
        second.original_mapped = sceGxmMapMemory(second.original_base, MIB, RW);
        PRINT("original mapped: map the original", second.original_mapped);
        PRINT("original mapped: free the original", sceKernelFreeMemBlock(second.original));
        lookup("original mapped", &second);
        second.opened_mapped = sceGxmMapMemory(second.opened_base, MIB, RW);
        PRINT("original mapped: map the opened block", second.opened_mapped);
    }

    // Last, since the unmaps may touch memory a wrong implementation freed
    if (have_first)
        clean_up("opened mapped", &first);
    if (have_second) {
        clean_up("original mapped", &second);
        const SceUID found = sceKernelFindMemBlockByAddr(second.original_base, 0);
        PRINT("original mapped, after the clean up: find the original by its address",
              found >= 0 ? 0 : found);
        AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
        const SceUID again = sceKernelAllocMemBlock(second_name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                                    MIB, (SceKernelAllocMemBlockOpt *)&opt);
        PRINT("original mapped, after the clean up: allocate a block with the original's name",
              test_created(again));
        if (again >= 0)
            sceKernelFreeMemBlock(again);
    }
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
