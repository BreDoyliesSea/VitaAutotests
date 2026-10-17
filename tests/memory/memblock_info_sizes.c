// The struct sizes sceKernelGetMemBlockInfoByAddr, sceKernelGetMemBlockInfoByRange and
// sceKernelGetFreeMemorySize take, and which words of the struct each call writes. The struct is
// at the start of a 1 KiB buffer, every word of it set to a marker but the first, which holds the
// size, so a call that writes past the struct changes only the buffer. Each case prints the result
// and a mask of the words the call changed: bit n for word n of the first 16, bit 0 being the
// size, and bit 16 for any word after those. The block is 64 KiB of USER_RW, and the info by range
// call asks for its first page. Then which error comes first, the size's or the address's, for the
// base of a block just freed. Last, a size of 0xFFFFFFFF for each call and a NULL pointer for the
// info by range call, so an implementation that crashes on them loses nothing else.
//
// On a PS Vita TV on firmware 3.65 both info calls take only the size 0x18 and the free size call
// only 0x10, every other size giving 0x80020005 (INVALID_ARGUMENT). The info calls write words 1 to
// 5 and the free size call words 1 to 3, leaving the size and everything after the struct alone. An
// address in no block gives 0x80022007 whatever the size, so the address is checked first. The info
// call by range into NULL gives 0x80022005 (INVALID_MEMORY_ACCESS).
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MARKER 0xA5A5A5A5
#define WORDS 256
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static SceUInt32 buffer[WORDS];

static void fill(SceSize size) {
    buffer[0] = size;
    for (int i = 1; i < WORDS; i++)
        buffer[i] = MARKER;
}

static int changed(SceSize size) {
    int mask = 0;
    for (int i = 0; i < WORDS; i++)
        if (buffer[i] != (i == 0 ? size : MARKER))
            mask |= 1 << (i < 16 ? i : 16);
    return mask;
}

static void by_addr(const char *label, void *addr, SceSize size) {
    fill(size);
    PRINTF(sceKernelGetMemBlockInfoByAddr(addr, (SceKernelMemBlockInfo *)buffer), "%s, size 0x%X",
        label, size);
    PRINTF(changed(size), "%s, size 0x%X: the words it changed", label, size);
}

static void by_range(const char *label, void *addr, SceSize size) {
    fill(size);
    PRINTF(sceKernelGetMemBlockInfoByRange(addr, 4 * KIB, (SceKernelMemBlockInfo *)buffer),
        "%s, size 0x%X", label, size);
    PRINTF(changed(size), "%s, size 0x%X: the words it changed", label, size);
}

static void free_size(SceSize size) {
    fill(size);
    PRINTF(sceKernelGetFreeMemorySize((SceKernelFreeMemorySizeInfo *)buffer),
        "free memory size, size 0x%X", size);
    PRINTF(changed(size), "free memory size, size 0x%X: the words it changed", size);
}

int main(void) {
    const SceUID block
        = sceKernelAllocMemBlock("info sizes", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, NULL);
    PRINT("allocate the block", test_created(block));
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);

    static const SceSize info_sizes[] = { 0, 4, 0x14, 0x17, 0x18, 0x19, 0x1C, 0x20, 0x40, 0x100 };
    for (unsigned int i = 0; i < COUNT(info_sizes); i++)
        by_addr("info by address", base, info_sizes[i]);
    for (unsigned int i = 0; i < COUNT(info_sizes); i++)
        by_range("info by range", base, info_sizes[i]);
    static const SceSize free_sizes[] = { 0, 4, 0xC, 0xF, 0x10, 0x11, 0x14, 0x18, 0x20, 0x100 };
    for (unsigned int i = 0; i < COUNT(free_sizes); i++)
        free_size(free_sizes[i]);

    // The base of a block just freed is in no block, as nothing is made after the free
    const SceUID freed = sceKernelAllocMemBlock(
        "info sizes freed", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, NULL);
    void *freed_base = NULL;
    sceKernelGetMemBlockBase(freed, &freed_base);
    sceKernelFreeMemBlock(freed);
    by_addr("info by address in no block", freed_base, 0x18);
    by_addr("info by address in no block", freed_base, 0x14);
    by_range("info by range in no block", freed_base, 0x18);
    by_range("info by range in no block", freed_base, 0x14);

    by_addr("info by address", base, 0xFFFFFFFF);
    by_range("info by range", base, 0xFFFFFFFF);
    free_size(0xFFFFFFFF);
    PRINT("info by range into NULL", sceKernelGetMemBlockInfoByRange(base, 4 * KIB, NULL));

    sceKernelFreeMemBlock(block);
    test_exit();
    return 0;
}
