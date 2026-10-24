// The raw bits of the UIDs the thread manager hands out: the top two bits, the lowest bit and
// whether the UID reads as a positive number, for a semaphore, a simple event and a thread, and the
// top two bits and the lowest bit of the ID sceKernelGetThreadId returns. The UIDs themselves are
// never printed. The semaphore's lowest bit is left out, since uid_semantics prints it, and so are
// the raw errors of a wait on an unknown semaphore and of a delete of an unknown mutex, which
// sema_errors and mutex_errors print as the system returns them.
//
// On a PS Vita TV on firmware 3.65 every UID is a positive number with the top two bits 01, so
// uid >> 30 is 1, and every UID printed here is odd. The ID of the main thread has the same bits.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

int main(void) {
    const SceUID sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    PRINT("semaphore: the UID >> 30", (SceUInt32)sema >> 30);
    PRINT("semaphore: the UID is positive", sema > 0);
    sceKernelDeleteSema(sema);

    const SceUID event = sceKernelCreateSimpleEvent("event", 0, 0, NULL);
    PRINT("simple event: the UID >> 30", (SceUInt32)event >> 30);
    PRINT("simple event: the UID & 1", event & 1);
    PRINT("simple event: the UID is positive", event > 0);
    sceKernelDeleteSimpleEvent(event);

    const SceUID thread = sceKernelCreateThread("thread", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    PRINT("thread: the UID >> 30", (SceUInt32)thread >> 30);
    PRINT("thread: the UID & 1", thread & 1);
    PRINT("thread: the UID is positive", thread > 0);
    sceKernelDeleteThread(thread);

    const SceUID self = sceKernelGetThreadId();
    PRINT("sceKernelGetThreadId: the ID >> 30", (SceUInt32)self >> 30);
    PRINT("sceKernelGetThreadId: the ID & 1", self & 1);

    test_exit();
    return 0;
}
