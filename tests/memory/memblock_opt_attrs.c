// Which option sizes and attr bits sceKernelAllocMemBlock takes from an app. Each call asks for 4
// KiB of USER_RW with every other option field 0, and frees the block if it was made. For a call
// that makes one, also whether it's a plain block: 4 KiB mapped, the USER_RW type, at a 4 KiB
// aligned base. The cases:
// - option sizes from 0 to 0x48 in steps of 4, then 0x100, with attr 0
// - each of the 32 attr bits alone, with option size 0x14 and with 0x40, the size of the vitasdk
//   struct
//
// On a PS Vita TV on firmware 3.65 the option size must be 0x14, 0x38, 0x3C or 0x40, and any other
// fails with 0x80020009 (SCE_KERNEL_ERROR_INVALID_ARGUMENT_SIZE). Of the attr bits an app may only
// pass 0x4 (HAS_ALIGNMENT), 0x4000 and 0x200000 (PHYCONT), and each gives a plain block. Every
// other bit fails with 0x80020005 (SCE_KERNEL_ERROR_INVALID_ARGUMENT), with either option size.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400

// The option struct, with room for every size tried
typedef struct Opt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUID baseBlock;
    const char *baseBlockName;
    SceUInt32 extra[59];
} Opt;

// Whether block is a plain 4 KiB USER_RW block
static int is_plain(SceUID block) {
    void *base = NULL;
    if (sceKernelGetMemBlockBase(block, &base) < 0)
        return 0;
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    if (sceKernelGetMemBlockInfoByAddr(base, &info) < 0)
        return 0;
    return info.mappedBase == base && info.mappedSize == 4 * KIB && info.type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW
        && (SceUIntPtr)base % (4 * KIB) == 0;
}

// Allocates with the option size and attr, and prints the result, and whether the block is plain
static void try_opt(const char *label, SceSize size, SceUInt32 attr) {
    Opt opt;
    sceClibMemset(&opt, 0, sizeof(opt));
    opt.size = size;
    opt.attr = attr;
    const SceUID block = sceKernelAllocMemBlock("opt", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    const int plain = block >= 0 && is_plain(block);
    if (block >= 0)
        sceKernelFreeMemBlock(block);
    PRINTF(test_created(block), "%s", label);
    if (block >= 0)
        PRINTF(plain, "%s: a plain block", label);
}

int main(void) {
    char label[64];
    for (SceSize size = 0; size <= 0x48; size += 4) {
        sceClibSnprintf(label, sizeof(label), "option size 0x%X", size);
        try_opt(label, size, 0);
    }
    try_opt("option size 0x100", 0x100, 0);

    static const SceSize sizes[] = { 0x14, 0x40 };
    for (unsigned int s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        for (int bit = 0; bit < 32; bit++) {
            sceClibSnprintf(label, sizeof(label), "attr 0x%08X, option size 0x%X", 1u << bit, sizes[s]);
            try_opt(label, sizes[s], 1u << bit);
        }
    }
    test_exit();
    return 0;
}
