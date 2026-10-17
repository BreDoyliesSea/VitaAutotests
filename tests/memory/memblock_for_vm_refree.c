// Blocks for code from sceKernelAllocMemBlockForVM, past what memblock_for_vm and
// memblock_for_vm_space cover. Each step depends on the space the ones before it left, so their
// order is part of the test. The cases:
// - the lookups on a 1 MiB block: finds in the middle, over the whole block and over its last
//   page, the info call by range over the whole block, with the type it reports, and over a page
//   in the middle, and opening the block by its name
// - a 1 MiB block freed with the usual free: then its base, a find by its base, freeing it with
//   sceKernelFreeMemBlockForVM, whether a 16 MiB block fits after that, and freeing it with
//   sceKernelFreeMemBlockForVM again
// - the same for a 1 MiB block closed with sceKernelCloseMemBlock
// - names: empty, 31, 32 and 64 characters
// - last, whole MiB sizes past 16 MiB: 2 GiB, and 4095 MiB with nothing live and with a 1 MiB
//   block live, in case the check of the space adds the sizes and wraps around
//
// A find prints 0 when it gives the block, the error when it fails and 1 when it gives another
// UID. An info call prints its error, or 0 when it reports the block's base and size, and 1 when
// it reports something else.
//
// On a PS Vita TV on firmware 3.65 a block for code is found with size 0 and with its whole range,
// any other size fails with 0x80020001 like any block (memblock_find_ranges), the info call reports
// type 0x0C20D050, and the block can't be opened by its name (0x8002450A). Ended with the usual
// free or with a close, its UID is gone (0x80024501) and its address in no block.
// sceKernelFreeMemBlockForVM then fails with 0x80024501, but still gives the space back, since 16
// MiB fits after it, which the usual free alone doesn't (memblock_for_vm_space), and a second one
// gives 0x80020005, since the table no longer has the UID. Names of every length here work. 2 GiB
// and 4095 MiB give 0x80024B0B (MEMBLOCK_OVERFLOW), with or without a block live.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define NAME_31 "abcdefghijklmnopqrstuvwxyz01234"
#define NAME_32 NAME_31 "5"

// The stub library has it, but the headers have no prototype
int sceKernelFreeMemBlockForVM(SceUID uid);

static char *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

static int find_result(void *addr, SceSize size, SceUID block) {
    const SceUID found = sceKernelFindMemBlockByAddr(addr, size);
    return found == block ? 0 : found < 0 ? found : 1;
}

// The info call by range into info, printed as its error, or 0 when it reports the block at base
static int info_result(char *addr, SceSize size, char *base, SceKernelMemBlockInfo *info) {
    info->size = sizeof(*info);
    const int error = sceKernelGetMemBlockInfoByRange(addr, size, info);
    if (error < 0)
        return error;
    return info->mappedBase == base && info->mappedSize == MIB ? 0 : 1;
}

// Returns 0 if a block for code with the name and size is made, freeing it, or the error
static int alloc_result(const char *name, SceSize size) {
    const SceUID block = sceKernelAllocMemBlockForVM(name, size);
    if (block < 0)
        return block;
    sceKernelFreeMemBlockForVM(block);
    return 0;
}

static void lookups(void) {
    const SceUID block = sceKernelAllocMemBlockForVM("vm lookups", MIB);
    PRINT("lookups: allocate 1 MiB", test_created(block));
    if (block < 0)
        return;
    char *base = base_of(block);
    PRINT("lookups: find the middle", find_result(base + MIB / 2, 0, block));
    PRINT("lookups: find a byte in the middle", find_result(base + MIB / 2 + 1, 1, block));
    PRINT("lookups: find the whole block", find_result(base, MIB, block));
    PRINT("lookups: find the last page", find_result(base + MIB - 4 * KIB, 4 * KIB, block));
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    PRINT("lookups: info for the whole block", info_result(base, MIB, base, &info));
    PRINT("lookups: its type", info.type);
    PRINT("lookups: info for a page in the middle",
        info_result(base + MIB / 2, 4 * KIB, base, &info));
    const SceUID opened = sceKernelOpenMemBlock("vm lookups", 0x10);
    PRINT("lookups: open it by its name", test_created(opened));
    if (opened >= 0)
        sceKernelCloseMemBlock(opened);
    PRINT("lookups: free it with the call for code", sceKernelFreeMemBlockForVM(block));
}

// Ends a 1 MiB block with end, then frees it with the call for code and tries 16 MiB
static void ended(const char *how, int (*end)(SceUID)) {
    const SceUID block = sceKernelAllocMemBlockForVM("vm ended", MIB);
    PRINTF(test_created(block), "%s: allocate 1 MiB", how);
    if (block < 0)
        return;
    char *base = base_of(block);
    PRINTF(end(block), "%s: end it", how);
    void *out = NULL;
    PRINTF(sceKernelGetMemBlockBase(block, &out), "%s: get its base after that", how);
    PRINTF(find_result(base, 0, block), "%s: find by its base after that", how);
    PRINTF(sceKernelFreeMemBlockForVM(block), "%s: free it with the call for code after that", how);
    PRINTF(alloc_result("vm 16 MiB", 16 * MIB), "%s: allocate 16 MiB after that", how);
    PRINTF(sceKernelFreeMemBlockForVM(block), "%s: free it with the call for code again", how);
    PRINTF(alloc_result("vm 16 MiB", 16 * MIB), "%s: allocate 16 MiB after that", how);
}

int main(void) {
    lookups();
    ended("freed with the usual free", sceKernelFreeMemBlock);
    ended("closed", sceKernelCloseMemBlock);

    PRINT("allocate with an empty name", alloc_result("", MIB));
    PRINT("allocate with a 31 character name", alloc_result(NAME_31, MIB));
    PRINT("allocate with a 32 character name", alloc_result(NAME_32, MIB));
    PRINT("allocate with a 64 character name", alloc_result(NAME_32 NAME_32, MIB));

    // Huge sizes last, so an implementation that takes them loses nothing else
    PRINT("allocate 2 GiB", alloc_result("vm huge", 0x80000000));
    PRINT("allocate 4095 MiB", alloc_result("vm huge", 0xFFF00000));
    const SceUID live = sceKernelAllocMemBlockForVM("vm live", MIB);
    PRINT("allocate 1 MiB to keep", test_created(live));
    PRINT("allocate 4095 MiB with it live", alloc_result("vm huge", 0xFFF00000));
    if (live >= 0)
        sceKernelFreeMemBlockForVM(live);

    test_exit();
    return 0;
}
