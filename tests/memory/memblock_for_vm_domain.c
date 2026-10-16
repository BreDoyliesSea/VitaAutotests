// The VM domain and the calls for code across threads: allocating and freeing a block for code with
// the domain open, a NULL name, the allocation and sceKernelOpenVMDomain from a thread that started
// before another thread opened the domain and from one that started after, and a domain opened by
// one thread and closed by another, with what the first can do after that. No thread writes to a
// block for code, since a thread that didn't open the domain itself could fault doing so.
//
// a PS Vita TV on firmware 3.65 allocating and freeing a block for code work with the domain open,
// from any thread. The domain is one state for the process: opening it while it's open, from any
// thread, and closing it while it's closed fail with 0x80010058, and another thread may close it. A
// NULL name fails with 0x80020006 (SCE_KERNEL_ERROR_ILLEGAL_ADDR).
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define MIB 0x100000

// vitasdk has a stub for it but no prototype
int sceKernelFreeMemBlockForVM(SceUID uid);

static SceUID go;
static const char *label;

// Returns 0 if a 1 MiB block for code is made, freeing it, or the error
static int alloc_result(void) {
    const SceUID block = sceKernelAllocMemBlockForVM("vm probe", MIB);
    if (block < 0)
        return block;
    sceKernelFreeMemBlockForVM(block);
    return 0;
}

// Waits for the go, then tries the allocation and opening the domain
static int try_calls(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelWaitSema(go, 1, NULL);
    PRINTF(alloc_result(), "%s: allocate a block for code", label);
    PRINTF(sceKernelOpenVMDomain(), "%s: open the domain", label);
    return 0;
}

static int close_domain(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    PRINT("another thread: close the domain", sceKernelCloseVMDomain());
    return 0;
}

int main(void) {
    go = sceKernelCreateSema("go", 0, 0, 1, NULL);

    // One thread
    PRINT("allocate with a NULL name", test_created(sceKernelAllocMemBlockForVM(NULL, MIB)));
    const SceUID kept = sceKernelAllocMemBlockForVM("vm kept", MIB);
    PRINT("allocate a block for code", test_created(kept));
    PRINT("open the domain", sceKernelOpenVMDomain());
    PRINT("allocate another with the domain open", alloc_result());
    PRINT("free the block with the domain open", sceKernelFreeMemBlockForVM(kept));
    PRINT("close the domain", sceKernelCloseVMDomain());
    PRINT("free the block with the domain closed", sceKernelFreeMemBlockForVM(kept));

    // Other threads while this one has the domain open
    label = "a thread started before the open";
    SceUID thread = test_start_thread(try_calls);
    test_wait_until_waiting(thread);
    PRINT("open the domain for the other threads", sceKernelOpenVMDomain());
    sceKernelSignalSema(go, 1);
    test_join_thread(thread);
    label = "a thread started after the open";
    thread = test_start_thread(try_calls);
    sceKernelSignalSema(go, 1);
    test_join_thread(thread);
    PRINT("allocate with the domain open after them", alloc_result());
    PRINT("close the domain after them", sceKernelCloseVMDomain());

    // A domain another thread closes
    PRINT("open the domain again", sceKernelOpenVMDomain());
    thread = test_start_thread(close_domain);
    test_join_thread(thread);
    PRINT("allocate after another thread closed it", alloc_result());
    PRINT("open the domain after that", sceKernelOpenVMDomain());
    PRINT("close the domain after that", sceKernelCloseVMDomain());
    PRINT("close it again", sceKernelCloseVMDomain());

    sceKernelDeleteSema(go);
    test_exit();
    return 0;
}
