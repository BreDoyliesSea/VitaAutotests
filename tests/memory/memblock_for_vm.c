// Memory blocks for code from sceKernelAllocMemBlockForVM: which sizes it takes, what the info
// calls report about a block, which 256 MiB region its base is in, how much free memory it takes,
// whether a process may have two, syncing, opening and closing the domain, writing code into a
// block with the domain open and running it once the domain is closed and the code synced, and
// freeing blocks with both free calls. Freeing a block twice comes last, so an implementation that
// crashes on it loses nothing else.
//
// On a PS Vita TV on firmware 3.65 the size must be a whole number of MiB. Other sizes fail with
// 0x80024B05 (SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK_SIZE), 0 with 0x80020005
// (SCE_KERNEL_ERROR_INVALID_ARGUMENT) and more than 16 MiB with 0x80024B0B
// (SCE_KERNEL_ERROR_MEMBLOCK_OVERFLOW). A block has type 0x0C20D050, memory type 0xD0 and access
// 0x4, is in the 0x8 region and takes its size of main memory. sceKernelSyncVMDomain takes a data
// block too, and a range past the end of the block fails with 0x80024B09
// (SCE_KERNEL_ERROR_MEMBLOCK_RANGE_ERROR). Opening the domain while it's open or closing it while
// it's closed fails with 0x80010058, and the access stays 0x4 either way. Code written with the
// domain open runs once it's closed and synced. sceKernelFreeMemBlockForVM fails with 0x80020005
// for a freed block and for a data block, which it leaves alone. A sync with an unknown UID never
// returns, so the test leaves it out.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

// vitasdk has a stub for it but no prototype
int sceKernelFreeMemBlockForVM(SceUID uid);

// ARM code for a function that returns 42: mov r0, #42 and bx lr
static const SceUInt32 code[] = { 0xE3A0002A, 0xE12FFF1E };

// Returns 0 if a block for code of the size is made, or the error
static int alloc_result(SceSize size) {
    const SceUID block = sceKernelAllocMemBlockForVM("vm probe", size);
    if (block < 0)
        return block;
    sceKernelFreeMemBlockForVM(block);
    return 0;
}

static SceKernelMemBlockInfo info_of(void *base) {
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    const int error = sceKernelGetMemBlockInfoByAddr(base, &info);
    if (error < 0)
        PRINT("get the info", error);
    return info;
}

static void get_free_sizes(SceKernelFreeMemorySizeInfo *info) {
    info->size = sizeof(*info);
    const int error = sceKernelGetFreeMemorySize(info);
    if (error < 0)
        PRINT("get the free sizes", error);
}

int main(void) {
    // Sizes
    static const SceSize sizes[] = { 0, 4 * KIB, 64 * KIB, 512 * KIB, MIB - 4 * KIB, MIB, MIB + 4 * KIB, MIB + 1, 2 * MIB, 17 * MIB, 32 * MIB };
    for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        PRINTF(alloc_result(sizes[i]), "allocate 0x%X bytes", (unsigned)sizes[i]);

    // One block and what the calls report about it
    SceKernelFreeMemorySizeInfo before = { 0 }, during = { 0 };
    get_free_sizes(&before);
    const SceUID block = sceKernelAllocMemBlockForVM("vm", MIB);
    PRINT("allocate 1 MiB", test_created(block));
    if (block < 0) {
        test_exit();
        return 0;
    }
    get_free_sizes(&during);
    PRINT("main memory it takes", before.size_user - during.size_user);
    PRINT("CDRAM it takes", before.size_cdram - during.size_cdram);
    PRINT("physically contiguous memory it takes", before.size_phycont - during.size_phycont);

    void *base = NULL;
    PRINT("get the base", sceKernelGetMemBlockBase(block, &base));
    PRINT("its region", (SceUIntPtr)base >> 28);
    PRINT("the base is aligned to 4 KiB", (SceUIntPtr)base % (4 * KIB) == 0);
    PRINT("find it by its base", sceKernelFindMemBlockByAddr(base, 0) == block);
    SceKernelMemBlockInfo info = info_of(base);
    PRINT("its mapped base is the base", info.mappedBase == base);
    PRINT("its mapped size", info.mappedSize);
    PRINT("its memory type", info.memoryType);
    PRINT("its access", info.access);
    PRINT("its type", info.type);

    const SceUID second = sceKernelAllocMemBlockForVM("vm second", MIB);
    PRINT("allocate a second one while it lives", test_created(second));
    if (second >= 0)
        sceKernelFreeMemBlockForVM(second);

    // Syncing
    const SceUID data = sceKernelAllocMemBlock("data", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, NULL);
    void *data_base = NULL;
    sceKernelGetMemBlockBase(data, &data_base);
    PRINT("sync it with the domain closed", sceKernelSyncVMDomain(block, base, sizeof(code)));
    PRINT("sync a range past its end", sceKernelSyncVMDomain(block, (char *)base + MIB - 4, sizeof(code)));
    PRINT("sync a data block", sceKernelSyncVMDomain(data, data_base, sizeof(code)));
    sceKernelFreeMemBlock(data);

    // Opening and closing the domain
    PRINT("close the domain before opening it", sceKernelCloseVMDomain());
    PRINT("open the domain", sceKernelOpenVMDomain());
    info = info_of(base);
    PRINT("its access with the domain open", info.access);
    PRINT("open the domain again", sceKernelOpenVMDomain());
    PRINT("close the domain", sceKernelCloseVMDomain());
    info = info_of(base);
    PRINT("its access with the domain closed", info.access);
    PRINT("close the domain again", sceKernelCloseVMDomain());

    // Code written with the domain open runs once the domain is closed and the range synced
    PRINT("open the domain to write code", sceKernelOpenVMDomain());
    volatile SceUInt32 *words = base;
    words[0] = code[0];
    words[1] = code[1];
    PRINT("the code reads back", words[0] == code[0] && words[1] == code[1]);
    PRINT("close the domain to run it", sceKernelCloseVMDomain());
    PRINT("sync the code", sceKernelSyncVMDomain(block, base, sizeof(code)));
    int (*const function)(void) = (int (*)(void))base;
    PRINT("run the code", function());

    // Freeing
    PRINT("free it with the call for code", sceKernelFreeMemBlockForVM(block));
    PRINT("find it after the free", sceKernelFindMemBlockByAddr(base, 0) == block);
    get_free_sizes(&during);
    PRINT("the free gives it all back", before.size_user == during.size_user);
    PRINT("free it again", sceKernelFreeMemBlock(block));
    PRINT("free it again with the call for code", sceKernelFreeMemBlockForVM(block));
    const SceUID other = sceKernelAllocMemBlock("data", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, NULL);
    PRINT("free a data block with the call for code", sceKernelFreeMemBlockForVM(other));
    PRINT("free the data block with the usual call afterwards", sceKernelFreeMemBlock(other));

    test_exit();
    return 0;
}
