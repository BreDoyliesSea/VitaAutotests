// Whether UIDs opened from an object count toward the limit on a process's objects that
// object_limits finds. The test first creates semaphores until a create fails, at most 4096, and
// deletes them. Then it creates one openable semaphore and opens it until an open fails, at most
// 4096 times. It prints the number of creates less the number of opens, which is 1 when each
// opened UID takes the room of an object, then whether the opens reached the end of the loop, the
// open's error, whether a mutex can be created and the semaphore opened once more while the opened
// UIDs are there, and whether the creating UID still works. Then every opened UID is closed, and
// the test prints whether every close worked, and whether an open and a create work after.
//
// On a PS Vita TV on firmware 3.65 the opens stop before the loop's end with 0x8002450C
// (UID_MAX_OPEN), 1790 short of the number of creates, so the creates less the opens print 0x6FE
// and not 1. A mutex is still created while the opened UIDs are there, but one more open fails with
// UID_MAX_OPEN, and the creating UID still works. Every opened UID closes, and then an open and a
// create work again.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define LIMIT 4096

static SceUID uids[LIMIT];

int main(void) {
    // A line first, so the output file is open before the creates fill anything up
    const SceUID first = sceKernelCreateSema("first", 0, 0, 1, NULL);
    PRINT("create a semaphore before the others", test_created(first));
    sceKernelDeleteSema(first);

    int creates = 0;
    while (creates < LIMIT) {
        const SceUID id = sceKernelCreateSema("limit", 0, 0, 1, NULL);
        if (id < 0)
            break;
        uids[creates++] = id;
    }
    for (int i = 0; i < creates; i++)
        sceKernelDeleteSema(uids[i]);

    const SceUID sema = sceKernelCreateSema("opened", SCE_KERNEL_ATTR_OPENABLE, 0, 4, NULL);
    PRINT("create the openable semaphore", test_created(sema));
    int opens = 0, error = 0;
    while (opens < LIMIT) {
        const SceUID id = sceKernelOpenSema("opened");
        if (id < 0) {
            error = id;
            break;
        }
        uids[opens++] = id;
    }
    PRINT("the creates less the opens", creates - opens);
    PRINT("the opens reached the end of the loop", opens == LIMIT);
    PRINT("the open that failed", error);
    const SceUID mutex = sceKernelCreateMutex("other", 0, 0, NULL);
    PRINT("create a mutex while the opened UIDs are there", test_created(mutex));
    if (mutex >= 0)
        sceKernelDeleteMutex(mutex);
    const SceUID extra = sceKernelOpenSema("opened");
    PRINT("open the semaphore once more", test_created(extra));
    if (extra >= 0)
        sceKernelCloseSema(extra);
    PRINT("signal through the creating UID", sceKernelSignalSema(sema, 1));

    int all_closed = 1;
    for (int i = 0; i < opens; i++)
        all_closed &= sceKernelCloseSema(uids[i]) == 0;
    PRINT("every opened UID closes", all_closed);
    const SceUID again = sceKernelOpenSema("opened");
    PRINT("open it after", test_created(again));
    if (again >= 0)
        sceKernelCloseSema(again);
    const SceUID late = sceKernelCreateSema("late", 0, 0, 1, NULL);
    PRINT("create a semaphore after", test_created(late));
    if (late >= 0)
        sceKernelDeleteSema(late);
    sceKernelDeleteSema(sema);

    test_exit();
    return 0;
}
