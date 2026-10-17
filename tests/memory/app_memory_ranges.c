// sceKernelFindMemBlockByAddr and sceKernelGetMemBlockInfoByRange with ranges over the blocks
// behind an app's own memory, which sceKernelGetMemBlockInfoByAddr reports: the block of main, of
// a global, of the main thread's stack, of its TLS and of newlib's heap. For each block: the whole
// block, its first and last page, a range from the object up to the block's end, and 4 bytes at
// the object. For the stack also the stack itself, from the base and size its thread info
// reports, and for a thread with a stack of 0x4000 that never starts, the same. No range reaches
// past a block, so nothing depends on what lies next to it.
//
// Last, sceKernelOpenMemBlock with flags 0x10 on the name of that thread, which the blocks of its
// stack and TLS may have, and on "Newlib heap", the name newlib gives its heap block. A block an
// open gives is closed again only if its base isn't the base of a block it names, so the test
// never closes memory the app runs on.
//
// A find prints 0 when it gives a UID and the error when it fails. An info call prints its error,
// or 0 when it reports the base and size the info call by address reports, and 1 otherwise.
//
// On a PS Vita TV on firmware 3.65 the info call by range reports the block for every range here.
// The find with a size gives a UID only for malloc's whole heap block. It fails with 0x80020001
// (ERROR) for every range that isn't a whole block, and for the whole code block and the whole
// block of each stack, and gives 0x80024501 (INVALID_UID) for the whole data block and for the TLS
// block's page, which is all of it. Neither the thread's name nor "Newlib heap" opens (0x8002450A).
#include <stdlib.h>

#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define KIB 0x400
#define THREAD_NAME "app memory ranges"

static volatile int data_word = 1;

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

static void check_range(const char *name, const char *what, SceUIntPtr addr, SceSize size,
    const SceKernelMemBlockInfo *block) {
    const SceUID uid = sceKernelFindMemBlockByAddr((const void *)addr, size);
    PRINTF(uid < 0 ? uid : 0, "%s, %s: find", name, what);
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    int result = sceKernelGetMemBlockInfoByRange((void *)addr, size, &info);
    if (result == 0)
        result = info.mappedBase == block->mappedBase && info.mappedSize == block->mappedSize
            ? 0
            : 1;
    PRINTF(result, "%s, %s: info by range", name, what);
}

// Prints the ranges over the block of addr, and returns its info call's result
static int check_block(const char *name, SceUIntPtr addr, SceKernelMemBlockInfo *block) {
    block->size = sizeof(*block);
    const int info = sceKernelGetMemBlockInfoByAddr((void *)addr, block);
    PRINTF(info, "%s: info by address", name);
    if (info < 0)
        return info;
    const SceUIntPtr base = (SceUIntPtr)block->mappedBase, end = base + block->mappedSize;
    check_range(name, "the whole block", base, block->mappedSize, block);
    check_range(name, "its first page", base, 4 * KIB, block);
    check_range(name, "its last page", end - 4 * KIB, 4 * KIB, block);
    check_range(name, "from it to the block's end", addr, end - addr, block);
    check_range(name, "4 bytes at it", addr, 4, block);
    return 0;
}

static void check_stack(const char *name, const SceKernelThreadInfo *info) {
    SceKernelMemBlockInfo block = { 0 };
    if (check_block(name, (SceUIntPtr)info->stack, &block) == 0)
        check_range(name, "the stack", (SceUIntPtr)info->stack, info->stackSize, &block);
}

// Opens name with flags 0x10, and closes what it gives unless it is a block at base or other
static void open_name(const char *label, const char *name, const void *base, const void *other) {
    const SceUID opened = sceKernelOpenMemBlock(name, 0x10);
    PRINTF(test_created(opened), "open %s", label);
    if (opened < 0)
        return;
    void *opened_base = NULL;
    sceKernelGetMemBlockBase(opened, &opened_base);
    const int elsewhere = opened_base != base && opened_base != other;
    PRINTF(elsewhere, "open %s: it maps at another address", label);
    if (elsewhere)
        PRINTF(sceKernelCloseMemBlock(opened), "open %s: close it", label);
}

int main(void) {
    SceKernelMemBlockInfo block = { 0 };
    check_block("main", (SceUIntPtr)main & ~(SceUIntPtr)1, &block);
    check_block("a global", (SceUIntPtr)&data_word, &block);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    check_stack("the main thread's stack", &info);
    check_block("the main thread's TLS", (SceUIntPtr)sceKernelGetTLSAddr(0), &block);
    void *heap = malloc(64);
    SceKernelMemBlockInfo heap_block = { 0 };
    check_block("memory from malloc", (SceUIntPtr)heap, &heap_block);

    const SceUID thread = sceKernelCreateThread(THREAD_NAME, entry,
        SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    SceKernelThreadInfo thread_info = { .size = sizeof(thread_info) };
    sceKernelGetThreadInfo(thread, &thread_info);
    check_stack("a new thread's stack", &thread_info);

    SceKernelMemBlockInfo tls_block = { .size = sizeof(tls_block) };
    sceKernelGetMemBlockInfoByAddr(sceKernelGetThreadTLSAddr(thread, 0), &tls_block);
    open_name("the thread's name", THREAD_NAME, thread_info.stack, tls_block.mappedBase);
    open_name("Newlib heap", "Newlib heap", heap_block.mappedBase, heap_block.mappedBase);

    sceKernelDeleteThread(thread);
    free(heap);
    test_exit();
    return 0;
}
