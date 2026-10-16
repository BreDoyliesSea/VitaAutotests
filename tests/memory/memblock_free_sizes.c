// Which free memory a block of each type an app may allocate takes: a 1 MiB block of each, how much
// the main, CDRAM and physically contiguous free sizes from sceKernelGetFreeMemorySize drop, and
// whether freeing the block gives it all back.
//
// On a PS Vita TV on firmware 3.65 a block of one of the three CDRAM types takes 1 MiB of CDRAM,
// one of the two PHYCONT types 1 MiB of physically contiguous memory, and a main, device or game
// one 1 MiB of main memory. A block of one of the six common dialog types takes none of the three.
// Freeing a block gives it all back.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define MIB 0x100000

// The types an app may allocate
static const SceUInt32 types[] = {
    SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_L1WBWA_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_R,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_DEVICE_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_GAME_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_NC_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_R,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_RW,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_NC_R,
    SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_NC_RW,
};

// Reads the free sizes, printing the error only if that fails
static void get_free_sizes(SceKernelFreeMemorySizeInfo *info) {
    info->size = sizeof(*info);
    const int error = sceKernelGetFreeMemorySize(info);
    if (error < 0)
        PRINT("get the free sizes", error);
}

static void check_type(SceUInt32 type) {
    SceKernelFreeMemorySizeInfo before = { 0 }, during = { 0 }, after = { 0 };
    get_free_sizes(&before);
    const SceUID block = sceKernelAllocMemBlock("free sizes", type, MIB, NULL);
    PRINTF(test_created(block), "type 0x%08X: allocate 1 MiB", (unsigned)type);
    if (block < 0)
        return;
    get_free_sizes(&during);
    const int freed = sceKernelFreeMemBlock(block);
    if (freed < 0)
        PRINTF(freed, "type 0x%08X: free it", (unsigned)type);
    get_free_sizes(&after);
    PRINTF(before.size_user - during.size_user, "type 0x%08X: main memory it takes", (unsigned)type);
    PRINTF(before.size_cdram - during.size_cdram, "type 0x%08X: CDRAM it takes", (unsigned)type);
    PRINTF(before.size_phycont - during.size_phycont, "type 0x%08X: physically contiguous memory it takes", (unsigned)type);
    PRINTF(before.size_user == after.size_user && before.size_cdram == after.size_cdram && before.size_phycont == after.size_phycont,
           "type 0x%08X: freeing it gives it all back", (unsigned)type);
}

int main(void) {
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        check_type(types[i]);

    test_exit();
    return 0;
}
