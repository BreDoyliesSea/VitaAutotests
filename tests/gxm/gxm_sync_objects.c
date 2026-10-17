// sceGxmSyncObjectCreate and sceGxmSyncObjectDestroy: when they work, how many sync objects libgxm
// holds at once, and their errors. No GPU work runs, so nothing ever waits on a sync object. The
// cases:
// - a create before sceGxmInitialize, destroyed if it works
// - one sync object created and destroyed
// - up to 4096 at once, until a create fails, with whether at least 16, 64, 256 and 1024 of them
//   and all of them worked, the error of the one that failed, and whether every destroy works. The
//   count itself isn't printed, in case it depends on what else the system holds.
// - a create once they're all gone
// - a create after sceGxmTerminate, destroyed if it works, and one after initializing again
// Last, a destroy of NULL and a create into a NULL pointer.
//
// On a PS Vita TV on firmware 3.65 a create before sceGxmInitialize and one after sceGxmTerminate
// fail with 0x805B0000 (UNINITIALIZED), and a create works again once libgxm is initialized again.
// At least 16 but fewer than 64 sync objects live at once. The create past them fails with
// 0x805B0017 (DRIVER), every destroy works, and a create works again once they're gone. A destroy
// of NULL and a create into a NULL pointer give 0x805B0004 (INVALID_POINTER).
#include <psp2/gxm.h>

#include "test.h"

#define MAX 4096

static SceGxmSyncObject *objects[MAX];

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

// Creates a sync object, prints the result and destroys one that was made, printing that too
static void create_once(const char *what) {
    SceGxmSyncObject *object = NULL;
    const int created = sceGxmSyncObjectCreate(&object);
    PRINTF(created, "%s", what);
    if (created == 0)
        PRINTF(sceGxmSyncObjectDestroy(object), "%s: destroy it", what);
}

static void many(void) {
    int count = 0, error = 0;
    while (count < MAX) {
        const int created = sceGxmSyncObjectCreate(&objects[count]);
        if (created != 0) {
            error = created;
            break;
        }
        count++;
    }
    PRINT("many: at least 16 work", count >= 16);
    PRINT("many: at least 64 work", count >= 64);
    PRINT("many: at least 256 work", count >= 256);
    PRINT("many: at least 1024 work", count >= 1024);
    PRINT("many: all 4096 work", count == MAX);
    PRINT("many: the error of the one that failed", error);
    int destroyed = 1;
    for (int i = 0; i < count; i++)
        destroyed &= sceGxmSyncObjectDestroy(objects[i]) == 0;
    PRINT("many: every destroy works", destroyed);
}

int main(void) {
    create_once("create before initializing");
    PRINT("initialize libgxm", initialize());
    create_once("create one");
    many();
    create_once("create one once they're gone");

    PRINT("terminate libgxm", sceGxmTerminate());
    create_once("create after terminating");
    PRINT("initialize libgxm again", initialize());
    create_once("create after initializing again");

    // NULL pointers go last, so an implementation that crashes on them loses nothing else
    PRINT("destroy NULL", sceGxmSyncObjectDestroy(NULL));
    PRINT("create into a NULL pointer", sceGxmSyncObjectCreate(NULL));
    PRINT("terminate libgxm again", sceGxmTerminate());
    test_exit();
    return 0;
}
