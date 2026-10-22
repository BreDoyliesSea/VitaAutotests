// What sceKernelAllocMemBlock does with HAS_ALIGNMENT and an alignment of 0, which
// memblock_alignment never tried. A program may set HAS_ALIGNMENT and leave the alignment field 0,
// and it fails to get its block if the call refuses that. Each case asks for one unit of a type: 4
// KiB of USER_RW and USER_RW_UNCACHE, 256 KiB of USER_CDRAM_RW, 1 MiB of USER_MAIN_PHYCONT_RW and
// of USER_MAIN_PHYCONT_NC_RW (type 0x0D808060), and last 1 MiB of USER_RW with the PHYCONT
// attribute (0x200000). It prints the result of the allocation, and if that made a block, whether
// the base is aligned to the unit. For comparison, USER_MAIN_PHYCONT_RW with an alignment of
// 0x100000 comes at the end, which memblock_alignment found to fail.
//
// On a PS Vita TV on firmware 3.65 every one of these takes HAS_ALIGNMENT with an alignment of 0,
// USER_MAIN_PHYCONT_RW and USER_MAIN_PHYCONT_NC_RW included, and gives a base aligned to the
// smallest unit of its type, while USER_MAIN_PHYCONT_RW still refuses an alignment of 0x100000 with
// 0x80020005.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000
#define PHYCONT 0x200000

typedef struct Opt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUID baseBlock;
    const char *baseBlockName;
} Opt;

// Prints the result of an allocation with HAS_ALIGNMENT and the alignment, and whether the base is
// aligned to the unit
static void try_alignment(const char *name, SceUInt32 type, SceSize unit, SceUInt32 extra_attr, SceSize alignment) {
    Opt opt = { .size = sizeof(opt), .attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT | extra_attr, .alignment = alignment };
    const SceUID block = sceKernelAllocMemBlock("aligned", type, unit, (SceKernelAllocMemBlockOpt *)&opt);
    void *base = NULL;
    // A base the lookup didn't give would pass any alignment
    int got_base = 0;
    if (block >= 0) {
        got_base = sceKernelGetMemBlockBase(block, &base) == 0 && base != NULL;
        sceKernelFreeMemBlock(block);
    }
    PRINTF(test_created(block), "%s aligned to 0x%X", name, alignment);
    if (block >= 0)
        PRINTF(got_base && (SceUIntPtr)base % unit == 0, "%s aligned to 0x%X: the base is aligned to 0x%X", name, alignment, unit);
}

int main(void) {
    try_alignment("USER_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, 0, 0);
    try_alignment("USER_RW_UNCACHE", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * KIB, 0, 0);
    try_alignment("USER_CDRAM_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB, 0, 0);
    try_alignment("USER_MAIN_PHYCONT_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, MIB, 0, 0);
    try_alignment("USER_MAIN_PHYCONT_NC_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, MIB, 0, 0);
    try_alignment("USER_RW with PHYCONT", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MIB, PHYCONT, 0);

    try_alignment("USER_MAIN_PHYCONT_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, MIB, 0, MIB);
    test_exit();
    return 0;
}
