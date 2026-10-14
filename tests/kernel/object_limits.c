// How many semaphores, and then mutexes, a process can have at once. Each type is created in a loop
// of at most 4096 until a create fails, which prints the count reached and the error, with 0 for
// no error. While the semaphores are all there, a mutex is created too, to see if the types share
// the limit. Then everything is deleted, and a create after that shows the room is back.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define LIMIT 4096

enum Kind { SEMA, MUTEX };
static const char *const kind_names[] = { "semaphores", "mutexes" };

static SceUID uids[LIMIT];

static SceUID create(int kind) {
    return kind == SEMA ? sceKernelCreateSema("limit", 0, 0, 1, NULL) : sceKernelCreateMutex("limit", 0, 0, NULL);
}

static int delete(int kind, SceUID id) {
    return kind == SEMA ? sceKernelDeleteSema(id) : sceKernelDeleteMutex(id);
}

int main(void) {
    // A line first, so the output file is open before the creates fill anything up
    const SceUID first = sceKernelCreateSema("first", 0, 0, 1, NULL);
    PRINT("create a semaphore before the others", test_created(first));
    sceKernelDeleteSema(first);

    for (int kind = SEMA; kind <= MUTEX; kind++) {
        const char *name = kind_names[kind];
        int count = 0, error = 0;
        while (count < LIMIT) {
            const SceUID id = create(kind);
            if (id < 0) {
                error = id;
                break;
            }
            uids[count++] = id;
        }
        PRINTF(count, "%s: created before an error or the end of the loop", name);
        PRINTF(error, "%s: the error", name);
        if (kind == SEMA) {
            const SceUID mutex = sceKernelCreateMutex("other", 0, 0, NULL);
            PRINTF(test_created(mutex), "%s: create a mutex while they are there", name);
            if (mutex >= 0)
                sceKernelDeleteMutex(mutex);
        }
        int all_deleted = 1;
        for (int i = 0; i < count; i++)
            all_deleted &= delete(kind, uids[i]) == 0;
        PRINTF(all_deleted, "%s: every one deletes", name);
        const SceUID again = create(kind);
        PRINTF(test_created(again), "%s: create one after deleting them", name);
        if (again >= 0)
            delete(kind, again);
    }

    test_exit();
    return 0;
}
