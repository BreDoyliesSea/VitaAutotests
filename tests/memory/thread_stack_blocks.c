// The memory blocks behind thread stacks. First the main thread's stack, then a new thread for each
// stack size: 0x100, 0x1000, 0x1234, which isn't 4 KiB aligned, 0x4000, 0x10000, 1 MiB and 1 MiB
// and 4 KiB. Each thread is created and looked at before it starts: the stack size its thread info
// reports, whether its stack base is 4 KiB aligned, what sceKernelFindMemBlockByAddr gives for the
// base, what sceKernelGetMemBlockInfoByAddr reports for it, how many bytes its block's size is
// over the reported stack size, whether the stack's last byte, the byte at its top and the byte
// below its base are in the same block, and sceKernelGetMemBlockInfoByRange over the stack. Then
// the thread is deleted without running, and the info call on its old base, made right after the
// delete with nothing made in between, prints its error.
//
// Then a thread with a stack of 0x4000 that runs: whether a local on its stack, looked up while it
// waits, is in the block found before it started, whether that block is still there once it
// ended, and the info call on the old base right after the delete. Last, how much of the main,
// CDRAM and physically contiguous free sizes a thread with an 8 MiB stack takes, with a margin of
// 1 MiB as the main free size moves in steps of 1 MiB, and whether they are back after the delete.
//
// A find prints 0 when it gives a UID and the error when it fails. An info call prints its error,
// and comparisons with a block print 0 when the info call failed.
//
// On a PS Vita TV on firmware 3.65 every stack, the main thread's too, is a block of its own of
// type 0x0C20D060, access R and W, which starts at the stack base and is exactly the stack size,
// the size asked rounded up to 4 KiB (0x100 gives 0x1000 and 0x1234 gives 0x2000). The find gives
// 0x80024501 (INVALID_UID) for it, since the process has no UID for it, while the info calls report
// it. A dormant thread already has its stack block, a thread keeps it while it runs and after it
// ends, and the delete ends it at once (0x80022007). An 8 MiB stack takes between 8 and 9 MiB of
// main memory and no CDRAM or physically contiguous memory, and the delete gives it back.
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define KIB 0x400
#define MIB 0x100000

typedef struct Found {
    int find;
    int info;
    SceKernelMemBlockInfo block;
} Found;

static SceUID sema;
static volatile SceUIntPtr local_address;

static Found look_up(SceUIntPtr addr) {
    Found found = { .block = { .size = sizeof(found.block) } };
    const SceUID uid = sceKernelFindMemBlockByAddr((const void *)addr, 0);
    found.find = uid < 0 ? uid : 0;
    found.info = sceKernelGetMemBlockInfoByAddr((void *)addr, &found.block);
    return found;
}

static int info_error(SceUIntPtr addr) {
    SceKernelMemBlockInfo block = { .size = sizeof(block) };
    return sceKernelGetMemBlockInfoByAddr((void *)addr, &block);
}

// Whether addr is in the block found, by the info call
static int in_block(SceUIntPtr addr, const Found *found) {
    SceKernelMemBlockInfo block = { .size = sizeof(block) };
    return found->info == 0 && sceKernelGetMemBlockInfoByAddr((void *)addr, &block) == 0
        && block.mappedBase == found->block.mappedBase;
}

static SceKernelThreadInfo thread_info(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info;
}

// Prints what the calls report about the block of a stack
static Found print_stack(const char *name, const SceKernelThreadInfo *info) {
    const SceUIntPtr base = (SceUIntPtr)info->stack;
    const SceSize size = info->stackSize;
    PRINTF(base % (4 * KIB) == 0, "%s: its stack base is 4 KiB aligned", name);
    const Found found = look_up(base);
    PRINTF(found.find, "%s: find by the stack base", name);
    PRINTF(found.info, "%s: info by the stack base", name);
    if (found.info == 0) {
        PRINTF(found.block.mappedBase == info->stack, "%s: the block starts at the stack base",
            name);
        PRINTF(found.block.mappedSize - size, "%s: bytes the block has over the stack size", name);
        PRINTF(found.block.type, "%s: its type", name);
        PRINTF(found.block.memoryType, "%s: its memory type", name);
        PRINTF(found.block.access, "%s: its access", name);
    }
    PRINTF(in_block(base + size - 1, &found), "%s: the stack's last byte is in the block", name);
    PRINTF(in_block(base + size, &found), "%s: the byte at the stack's top is in the block", name);
    PRINTF(in_block(base - 1, &found), "%s: the byte below the stack base is in the block", name);
    SceKernelMemBlockInfo range = { .size = sizeof(range) };
    const int by_range = sceKernelGetMemBlockInfoByRange(info->stack, size, &range);
    PRINTF(by_range, "%s: info by range over the stack", name);
    if (by_range == 0 && found.info == 0)
        PRINTF(range.mappedBase == found.block.mappedBase
                && range.mappedSize == found.block.mappedSize,
            "%s: info by range reports the same block", name);
    return found;
}

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    volatile int local = 0;
    local_address = (SceUIntPtr)&local;
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    return local;
}

static void check_size(SceSize stack_size) {
    char name[48];
    sceClibSnprintf(name, sizeof(name), "a stack of 0x%X", (unsigned)stack_size);
    const SceUID thread = sceKernelCreateThread("stack block", waiter,
        SCE_KERNEL_DEFAULT_PRIORITY_USER, stack_size, 0, 0, NULL);
    PRINTF(test_created(thread), "%s: create", name);
    if (thread < 0)
        return;
    const SceKernelThreadInfo info = thread_info(thread);
    PRINTF(info.stackSize, "%s: the stack size it reports", name);
    print_stack(name, &info);
    const int deleted = sceKernelDeleteThread(thread);
    const int after = info_error((SceUIntPtr)info.stack);
    PRINTF(deleted, "%s: delete", name);
    PRINTF(after, "%s: info by the old stack base after the delete", name);
}

static SceKernelFreeMemorySizeInfo free_sizes(void) {
    SceKernelFreeMemorySizeInfo sizes = { .size = sizeof(sizes) };
    sceKernelGetFreeMemorySize(&sizes);
    return sizes;
}

int main(void) {
    sema = sceKernelCreateSema("stack block", 0, 0, 1, NULL);

    const SceKernelThreadInfo main_info = thread_info(sceKernelGetThreadId());
    print_stack("the main thread", &main_info);

    static const SceSize sizes[] = { 0x100, 0x1000, 0x1234, 0x4000, 0x10000, MIB, MIB + 4 * KIB };
    for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        check_size(sizes[i]);

    // A thread that runs
    const SceUID thread = sceKernelCreateThread("stack block run", waiter,
        SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    const SceKernelThreadInfo info = thread_info(thread);
    const Found before = look_up((SceUIntPtr)info.stack);
    local_address = 0;
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema));
    const SceKernelThreadInfo running = thread_info(thread);
    PRINT("a thread that runs: its stack base is the one it had before it started",
        running.stack == info.stack);
    PRINT("a thread that runs: its local is in the block found before it started",
        local_address && in_block(local_address, &before));
    sceKernelSignalSema(sema, 1);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    const Found ended = look_up((SceUIntPtr)info.stack);
    PRINT("a thread that ended: info by the stack base", ended.info);
    PRINT("a thread that ended: the same block as before it started",
        before.info == 0 && ended.info == 0 && ended.block.mappedBase == before.block.mappedBase);
    const int deleted = sceKernelDeleteThread(thread);
    const int after = info_error((SceUIntPtr)info.stack);
    PRINT("a thread that ended: delete", deleted);
    PRINT("a thread that ended: info by the old stack base after the delete", after);

    // The memory an 8 MiB stack takes
    const SceKernelFreeMemorySizeInfo start = free_sizes();
    const SceUID big = sceKernelCreateThread("stack block big", waiter,
        SCE_KERNEL_DEFAULT_PRIORITY_USER, 8 * MIB, 0, 0, NULL);
    const SceKernelFreeMemorySizeInfo with_big = free_sizes();
    PRINT("an 8 MiB stack: create", test_created(big));
    const int main_taken = start.size_user - with_big.size_user;
    PRINT("an 8 MiB stack: it takes at least 8 MiB of main memory", main_taken >= 8 * MIB);
    PRINT("an 8 MiB stack: it takes at most 9 MiB of main memory", main_taken <= 9 * MIB);
    PRINT("an 8 MiB stack: it takes no CDRAM", start.size_cdram == with_big.size_cdram);
    PRINT("an 8 MiB stack: it takes no physically contiguous memory",
        start.size_phycont == with_big.size_phycont);
    PRINT("an 8 MiB stack: delete", sceKernelDeleteThread(big));
    const SceKernelFreeMemorySizeInfo end = free_sizes();
    PRINT("an 8 MiB stack: the free sizes are back after the delete",
        end.size_user == start.size_user && end.size_cdram == start.size_cdram
            && end.size_phycont == start.size_phycont);

    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}
