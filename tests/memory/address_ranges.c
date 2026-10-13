// Where things are in an app's address space: which 256 MiB region holds the app's code and data,
// the main thread's and a new thread's stack, the TLS, and each kind of memory block. Only the
// region is printed, the top 4 bits of an address, so the output doesn't depend on exact addresses.
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

static int data_word;
static volatile SceUIntPtr thread_stack, thread_frame;

static int region(const void *address) {
    return (int)((SceUIntPtr)address >> 28);
}

static int stack_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    volatile int local = 0;
    thread_frame = (SceUIntPtr)&local;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
    thread_stack = (SceUIntPtr)info.stack;
    return 0;
}

static void print_block(const char *name, SceKernelMemBlockType type, SceSize size) {
    const SceUID block = sceKernelAllocMemBlock(name, type, size, NULL);
    PRINTF(test_created(block), "%s: allocate", name);
    if (block < 0)
        return;
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    PRINTF(region(base), "%s: the region of its base", name);
    PRINTF(region((char *)base + size - 1), "%s: the region of its last byte", name);
    sceKernelFreeMemBlock(block);
}

int main(void) {
    volatile int local = 0;
    PRINT("the code", region((const void *)main));
    PRINT("the data", region(&data_word));
    PRINT("the main thread's stack", region((const void *)&local));
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
    PRINT("the main thread's stack per its info", region(info.stack));
    PRINT("the main thread's TLS", region(sceKernelGetTLSAddr(0)));

    const SceUID thread = test_start_thread(stack_thread);
    test_join_thread(thread);
    PRINT("a new thread's stack", region((const void *)thread_stack));
    PRINT("its frame is on it", thread_frame >= thread_stack && thread_frame < thread_stack + 0x4000);

    print_block("USER_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 0x1000);
    print_block("USER_RW_UNCACHE", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 0x1000);
    print_block("USER_CDRAM_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 0x40000);
    print_block("USER_MAIN_PHYCONT_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, 0x100000);
    print_block("USER_MAIN_PHYCONT_NC_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 0x100000);
    print_block("a 16 MiB USER_RW block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 0x1000000);

    test_exit();
    return 0;
}
