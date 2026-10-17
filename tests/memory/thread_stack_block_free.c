// Whether an app can end the memory block behind a thread's stack, from a thread with a stack of
// 0x4000 that never starts. First the lookups: sceKernelFindMemBlockByAddr by its stack base, by
// the whole stack and by its TLS slot 0, and for a UID a find gives, sceKernelGetMemBlockBase on
// it and whether that is the stack base. Last, if the find by the stack base gave a UID,
// sceKernelFreeMemBlock and sceKernelCloseMemBlock on it, and then deleting the thread. The thread
// never starts, since its stack may be gone, and the free is last, since an implementation that
// frees the stack may crash on the delete. A find prints 0 when it gives a UID and the error when
// it fails.
//
// On a PS Vita TV on firmware 3.65 neither a dormant thread's stack nor its TLS gives a UID: the
// find gives 0x80024501 (INVALID_UID) for the stack base and TLS slot 0, and 0x80020001 (ERROR) for
// the whole stack, so there is nothing to free or close, and the delete works.
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

static SceUID find(const char *name, const void *addr, SceSize size, const void *stack) {
    const SceUID uid = sceKernelFindMemBlockByAddr(addr, size);
    PRINTF(uid < 0 ? uid : 0, "%s: find", name);
    if (uid >= 0) {
        void *base = NULL;
        PRINTF(sceKernelGetMemBlockBase(uid, &base), "%s: the base of the UID", name);
        PRINTF(base == stack, "%s: that base is the stack base", name);
    }
    return uid;
}

int main(void) {
    const SceUID thread = sceKernelCreateThread("stack block free", entry,
        SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    PRINT("create the thread", test_created(thread));
    if (thread < 0) {
        test_exit();
        return 0;
    }
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    const SceUID uid = find("the stack base", info.stack, 0, info.stack);
    find("the whole stack", info.stack, info.stackSize, info.stack);
    find("TLS slot 0", sceKernelGetThreadTLSAddr(thread, 0), 0, info.stack);

    if (uid >= 0) {
        PRINT("free the block found by the stack base", sceKernelFreeMemBlock(uid));
        PRINT("close it", sceKernelCloseMemBlock(uid));
    }
    PRINT("delete the thread", sceKernelDeleteThread(thread));
    test_exit();
    return 0;
}
