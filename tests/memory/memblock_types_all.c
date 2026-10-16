// Which memory block types sceKernelAllocMemBlock takes from an app: the type codes the Vita
// Development Wiki lists for 3.60, user and kernel ones, and a few made up from the user RW type
// with other access and cache bits. Each case asks for one unit, 1 MiB for a physically contiguous
// source, 256 KiB for a CDRAM one and 4 KiB for the rest. For a block it makes: its type, memory
// type and access from the info call, whether its mapped size is the unit, and which 256 MiB region
// its base is in. The blocks are freed without being touched, since some can't be read or written.
//
// On a PS Vita TV on firmware 3.65 an app gets 16 of these types: the three CDRAM ones, in the 0x6
// region, the uncached main ones in 0x7, the cached main ones in 0x8, the cached common dialog ones
// in 0xD and the uncached common dialog ones in 0x6. Every RX type fails, as do the kernel types
// and the made up ones, with 0x80024702 (SCE_KERNEL_ERROR_ILLEGAL_BLOCK_TYPE), while the tool types
// and 0x0E208050 fail with 0x80024B01 (SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK_TYPE).
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

static const SceUInt32 codes[] = {
    // User types
    0x09404060, 0x09408040, 0x09408060, 0x0C200860, 0x0C20D040, 0x0C20D060, 0x0C208060, 0x0C50D060,
    0x0C80D060, 0x0D808060, 0x0CA0D060, 0x0CA08060, 0x0CF0D060, 0x0CF08060, 0x0E20D040, 0x0E20D060,
    0x0E208040, 0x0E208060, 0x0E208050, 0x05408060, 0x0620D006,
    // User address space types the wiki lists as for the kernel
    0x0320D040, 0x0320D050, 0x0320D060, 0x0390D040, 0x0390D050, 0x0390D060, 0x03A0D040, 0x03A0D050,
    0x03A0D060, 0x03F0D040, 0x03F0D050, 0x03F0D060, 0x08F0D040, 0x08F0D050, 0x08F0D060, 0x08F0D006,
    0x0C20D050, 0x0C80D040, 0x0D808040, 0x0CF0D050, 0x0E20D050, 0x0720D006, 0x0B100240, 0x0B100260,
    0x0B100840, 0x0B100860,
    // Kernel types
    0x10200206, 0x10200806, 0x10204006, 0x10208004, 0x10208006, 0x1020D004, 0x1020D005, 0x1020D006,
    0x1020D007, 0x1080D004, 0x1080D006, 0x3080D004, 0x3080D006, 0x30808004, 0x30808006, 0x1050D006,
    0x10A0D004, 0x10A0D006, 0x10C08006, 0x10C0D006, 0x10F0D004, 0x10F0D005, 0x10F0D006, 0x10F08004,
    0x10F08006, 0x11208006, 0x1220D004, 0x1220D006, 0x13208004, 0x13208006, 0x20100204, 0x20100206,
    0x20100804, 0x20100806, 0x20108004, 0x20108006, 0x40404006, 0x40408006, 0x50408006, 0x60208004,
    0x60208006, 0x6020D004, 0x6020D006, 0x6050D006, 0x60208014, 0x60208016, 0x60F0D006, 0xA0408006,
    0xC050D006, 0xC0F0D006,
    // USER_RW with other access and cache bits
    0x0C20D000, 0x0C20D010, 0x0C20D020, 0x0C20D070, 0x0C20D066, 0x0C202060, 0x0C204060, 0x0C20E060,
};

// The unit for a type, from its source partition bits
static SceSize unit_of(SceUInt32 type) {
    switch ((type >> 20) & 0xF) {
    case 0x4:
        return 256 * KIB;
    case 0x8:
        return MIB;
    default:
        return 4 * KIB;
    }
}

int main(void) {
    for (unsigned int i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        const SceUInt32 type = codes[i];
        const SceSize unit = unit_of(type);
        const SceUID block = sceKernelAllocMemBlock("type", type, unit, NULL);
        SceKernelMemBlockInfo info = { .size = sizeof(info) };
        void *base = NULL;
        if (block >= 0) {
            sceKernelGetMemBlockBase(block, &base);
            sceKernelGetMemBlockInfoByAddr(base, &info);
            sceKernelFreeMemBlock(block);
        }
        PRINTF(test_created(block), "type 0x%08X", type);
        if (block < 0)
            continue;
        PRINTF(info.type, "type 0x%08X: its type", type);
        PRINTF(info.memoryType, "type 0x%08X: its memory type", type);
        PRINTF(info.access, "type 0x%08X: its access", type);
        PRINTF(info.mappedSize == unit, "type 0x%08X: its mapped size is the unit", type);
        PRINTF((SceUIntPtr)base >> 28, "type 0x%08X: its region", type);
    }
    test_exit();
    return 0;
}
