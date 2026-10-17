// How the free sizes from sceKernelGetFreeMemorySize step as blocks are made, with sizes that are
// whole MiB and sizes that aren't. Whether the main, CDRAM and physically contiguous free sizes
// are each a whole number of MiB at the start. Then for USER_RW blocks of 4 KiB, 512 KiB, 1 MiB
// less 4 KiB, 1 MiB, 1 MiB and 4 KiB, 1 MiB and 512 KiB, 2 MiB less 4 KiB, 2 MiB and 16 MiB, and
// USER_CDRAM_RW blocks of 256 KiB, 512 KiB, 768 KiB, 1 MiB, 1 MiB and 256 KiB and 2 MiB, each
// made alone: the KiB it takes from its free size, and whether the free sizes are still whole
// MiB with it. Then 256 USER_RW blocks of 4 KiB, 2 of 512 KiB and 4 USER_CDRAM_RW blocks of 256
// KiB, each set live at once, with the same. Last, whether every free size is back to what it
// was at the start.
//
// On a PS Vita TV on firmware 3.65 every free size is a whole number of MiB at the start and with
// every block. A lone 4 KiB block took nothing, 512 KiB, 1 MiB less 4 KiB, 1 MiB and 1 MiB and 4
// KiB each took 1 MiB, the sizes up to 2 MiB took 2 MiB, and 16 MiB took 16 MiB. 256 blocks of 4
// KiB live at once took 1 MiB, and so did 2 blocks of 512 KiB. CDRAM steps the same way: 256 KiB up
// to 1 MiB took 1 MiB, more took 2 MiB, and 4 blocks of 256 KiB took 1 MiB. Every free size is back
// after the frees. That fits free sizes that count pages and are reported rounded down to a MiB,
// with the main free size between 4 KiB and 512 KiB past a whole MiB at the start, and
// app_free_sizes measures a 64 KiB block taking exactly 64 KiB.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000
#define MAX_BLOCKS 256

static SceUID blocks[MAX_BLOCKS];

static SceKernelFreeMemorySizeInfo free_sizes(void) {
    SceKernelFreeMemorySizeInfo info = { .size = sizeof(info) };
    const int error = sceKernelGetFreeMemorySize(&info);
    if (error < 0)
        PRINT("get the free sizes", error);
    return info;
}

static int whole(const SceKernelFreeMemorySizeInfo *info) {
    return info->size_user % MIB == 0 && info->size_cdram % MIB == 0
        && info->size_phycont % MIB == 0;
}

// Makes count blocks of the type and size at once, and prints the KiB they take from the main or
// CDRAM free size and whether the free sizes stay whole MiB
static void take(const char *label, SceUInt32 type, SceSize size, int count) {
    const SceKernelFreeMemorySizeInfo before = free_sizes();
    SceUID error = 0;
    for (int i = 0; i < count; i++) {
        blocks[i] = sceKernelAllocMemBlock("steps", type, size, NULL);
        if (blocks[i] < 0 && error == 0)
            error = blocks[i];
    }
    const SceKernelFreeMemorySizeInfo during = free_sizes();
    for (int i = 0; i < count; i++)
        if (blocks[i] >= 0)
            sceKernelFreeMemBlock(blocks[i]);
    PRINTF(error, "%s: make it", label);
    if (error < 0)
        return;
    const int cdram = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
    const int taken
        = cdram ? before.size_cdram - during.size_cdram : before.size_user - during.size_user;
    PRINTF(taken / KIB, "%s: KiB taken from %s", label, cdram ? "CDRAM" : "main");
    PRINTF(whole(&during), "%s: the free sizes are whole MiB with it", label);
}

int main(void) {
    const SceKernelFreeMemorySizeInfo start = free_sizes();
    PRINT("the main free size is whole MiB", start.size_user % MIB == 0);
    PRINT("the CDRAM free size is whole MiB", start.size_cdram % MIB == 0);
    PRINT("the physically contiguous free size is whole MiB", start.size_phycont % MIB == 0);

    static const struct {
        const char *label;
        SceSize size;
    } main_sizes[] = {
        { "USER_RW, 4 KiB", 4 * KIB },
        { "USER_RW, 512 KiB", 512 * KIB },
        { "USER_RW, 1 MiB less 4 KiB", MIB - 4 * KIB },
        { "USER_RW, 1 MiB", MIB },
        { "USER_RW, 1 MiB and 4 KiB", MIB + 4 * KIB },
        { "USER_RW, 1 MiB and 512 KiB", MIB + 512 * KIB },
        { "USER_RW, 2 MiB less 4 KiB", 2 * MIB - 4 * KIB },
        { "USER_RW, 2 MiB", 2 * MIB },
        { "USER_RW, 16 MiB", 16 * MIB },
    };
    for (unsigned int i = 0; i < sizeof(main_sizes) / sizeof(main_sizes[0]); i++)
        take(main_sizes[i].label, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, main_sizes[i].size, 1);

    static const struct {
        const char *label;
        SceSize size;
    } cdram_sizes[] = {
        { "USER_CDRAM_RW, 256 KiB", 256 * KIB },
        { "USER_CDRAM_RW, 512 KiB", 512 * KIB },
        { "USER_CDRAM_RW, 768 KiB", 768 * KIB },
        { "USER_CDRAM_RW, 1 MiB", MIB },
        { "USER_CDRAM_RW, 1 MiB and 256 KiB", MIB + 256 * KIB },
        { "USER_CDRAM_RW, 2 MiB", 2 * MIB },
    };
    for (unsigned int i = 0; i < sizeof(cdram_sizes) / sizeof(cdram_sizes[0]); i++)
        take(cdram_sizes[i].label, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, cdram_sizes[i].size, 1);

    take("256 USER_RW blocks of 4 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, 256);
    take("2 USER_RW blocks of 512 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 512 * KIB, 2);
    take("4 USER_CDRAM_RW blocks of 256 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB, 4);

    const SceKernelFreeMemorySizeInfo end = free_sizes();
    PRINT("every free size is back to the start",
        end.size_user == start.size_user && end.size_cdram == start.size_cdram
            && end.size_phycont == start.size_phycont);
    test_exit();
    return 0;
}
