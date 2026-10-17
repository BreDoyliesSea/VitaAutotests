// Deleting a thread while sceGxmMapMemory has its stack mapped. The threads have a stack of 0x4000
// and never start, and the mapping is the whole block sceKernelGetMemBlockInfoByAddr reports for
// the stack base, or the stack from its thread info if the info call fails. No GPU work runs.
// First a thread whose stack is mapped and unmapped before the delete, with the info call and
// sceKernelFindMemBlockByAddr on its old stack base after the delete. Then a thread deleted while
// its stack is mapped: the delete, the same lookups, the unmap, and the lookups again. That goes
// last, since an implementation that frees the stack on the delete may crash on the unmap.
//
// A find prints 0 when it gives a UID and the error when it fails.
//
// On a PS Vita TV on firmware 3.65 a stack unmapped before the delete is gone after it
// (0x80022007). A stack still mapped at the delete stays: the delete works, but the info call still
// reports the block at the old stack base and the find gives 0x80024501 (INVALID_UID), before and
// after the unmap, like a memory block freed while mapped (gxm_map_memory).
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

typedef struct Lookups {
    int info;
    int find;
} Lookups;

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

static void display_callback(const void *data) {
    (void)data;
}

static int initialize(void) {
    SceGxmInitializeParams init = { 0 };
    init.displayQueueMaxPendingCount = 2;
    init.displayQueueCallback = display_callback;
    init.displayQueueCallbackDataSize = 4;
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    return sceGxmInitialize(&init);
}

static Lookups look_up(void *addr) {
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    const SceUID uid = sceKernelFindMemBlockByAddr(addr, 0);
    const Lookups lookups = { sceKernelGetMemBlockInfoByAddr(addr, &info), uid < 0 ? uid : 0 };
    return lookups;
}

static void print_lookups(const char *when, const Lookups *lookups) {
    PRINTF(lookups->info, "%s: info by the old stack base", when);
    PRINTF(lookups->find, "%s: find by the old stack base", when);
}

typedef struct Stack {
    SceUID thread;
    void *base;
    SceSize size;
} Stack;

// Creates a thread and finds the range of its stack's block
static Stack create(void) {
    Stack stack = { .thread = sceKernelCreateThread("gxm stack delete", entry,
                        SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL) };
    SceKernelThreadInfo thread = { .size = sizeof(thread) };
    sceKernelGetThreadInfo(stack.thread, &thread);
    SceKernelMemBlockInfo block = { .size = sizeof(block) };
    if (sceKernelGetMemBlockInfoByAddr(thread.stack, &block) == 0) {
        stack.base = block.mappedBase;
        stack.size = block.mappedSize;
    } else {
        stack.base = thread.stack;
        stack.size = thread.stackSize;
    }
    return stack;
}

int main(void) {
    PRINT("initialize libgxm", initialize());

    const Stack first = create();
    PRINT("unmapped before the delete: create", test_created(first.thread));
    PRINT("unmapped before the delete: map the stack", sceGxmMapMemory(first.base, first.size, RW));
    PRINT("unmapped before the delete: unmap it", sceGxmUnmapMemory(first.base));
    const int first_deleted = sceKernelDeleteThread(first.thread);
    const Lookups after_first = look_up(first.base);
    PRINT("unmapped before the delete: delete", first_deleted);
    print_lookups("unmapped before the delete: after the delete", &after_first);

    // Last, since the unmap may crash where the delete frees a mapped stack
    const Stack second = create();
    PRINT("mapped at the delete: create", test_created(second.thread));
    const int mapped = sceGxmMapMemory(second.base, second.size, RW);
    PRINT("mapped at the delete: map the stack", mapped);
    const int second_deleted = sceKernelDeleteThread(second.thread);
    const Lookups after_delete = look_up(second.base);
    PRINT("mapped at the delete: delete", second_deleted);
    print_lookups("mapped at the delete: after the delete", &after_delete);
    if (mapped == 0) {
        PRINT("mapped at the delete: unmap it", sceGxmUnmapMemory(second.base));
        const Lookups after_unmap = look_up(second.base);
        print_lookups("mapped at the delete: after the unmap", &after_unmap);
    }

    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
