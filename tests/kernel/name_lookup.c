// Opening each sync object type by name: an exact name, names that differ in case or length, the
// empty name, a name only an object of another type has, an object without the openable attribute,
// and a deleted object. It also checks which names two objects may share, with and without the
// openable attribute and across types.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Kind { SEMA, EVENT_FLAG, MUTEX, COND, RWLOCK, MSG_PIPE, SIMPLE_EVENT, TIMER, KIND_COUNT };
static const char *const kind_names[] = {
    "semaphore", "event flag", "mutex", "condition variable", "reader/writer lock", "message pipe", "simple event", "timer",
};

static SceUID cond_mutex;

static SceUID create_object(int kind, const char *name, SceUInt32 attr) {
    switch (kind) {
    case SEMA: return sceKernelCreateSema(name, attr, 0, 1, NULL);
    case EVENT_FLAG: return sceKernelCreateEventFlag(name, attr, 0, NULL);
    case MUTEX: return sceKernelCreateMutex(name, attr, 0, NULL);
    case COND: return sceKernelCreateCond(name, attr, cond_mutex, NULL);
    case RWLOCK: return sceKernelCreateRWLock(name, attr, NULL);
    case MSG_PIPE: return sceKernelCreateMsgPipe(name, MSG_PIPE_TYPE_USER, attr, 16, NULL);
    case SIMPLE_EVENT: return sceKernelCreateSimpleEvent(name, attr, 0, NULL);
    case TIMER: return sceKernelCreateTimer(name, attr, NULL);
    }
    return -1;
}

static SceUID open_object(int kind, const char *name) {
    switch (kind) {
    case SEMA: return sceKernelOpenSema(name);
    case EVENT_FLAG: return sceKernelOpenEventFlag(name);
    case MUTEX: return sceKernelOpenMutex(name);
    case COND: return sceKernelOpenCond(name);
    case RWLOCK: return sceKernelOpenRWLock(name);
    case MSG_PIPE: return sceKernelOpenMsgPipe(name);
    case SIMPLE_EVENT: return sceKernelOpenSimpleEvent(name);
    case TIMER: return sceKernelOpenTimer(name);
    }
    return -1;
}

static int close_object(int kind, SceUID id) {
    switch (kind) {
    case SEMA: return sceKernelCloseSema(id);
    case EVENT_FLAG: return sceKernelCloseEventFlag(id);
    case MUTEX: return sceKernelCloseMutex(id);
    case COND: return sceKernelCloseCond(id);
    case RWLOCK: return sceKernelCloseRWLock(id);
    case MSG_PIPE: return sceKernelCloseMsgPipe(id);
    case SIMPLE_EVENT: return sceKernelCloseSimpleEvent(id);
    case TIMER: return sceKernelCloseTimer(id);
    }
    return -1;
}

static int delete_object(int kind, SceUID id) {
    switch (kind) {
    case SEMA: return sceKernelDeleteSema(id);
    case EVENT_FLAG: return sceKernelDeleteEventFlag(id);
    case MUTEX: return sceKernelDeleteMutex(id);
    case COND: return sceKernelDeleteCond(id);
    case RWLOCK: return sceKernelDeleteRWLock(id);
    case MSG_PIPE: return sceKernelDeleteMsgPipe(id);
    case SIMPLE_EVENT: return sceKernelDeleteSimpleEvent(id);
    case TIMER: return sceKernelDeleteTimer(id);
    }
    return -1;
}

// Opens name with the open call of the kind, prints the result, and closes what it opened
static void print_open(int kind, const char *name, const char *what) {
    const SceUID opened = open_object(kind, name);
    PRINTF(test_created(opened), "%s: open %s", kind_names[kind], what);
    if (opened >= 0)
        close_object(kind, opened);
}

int main(void) {
    cond_mutex = sceKernelCreateMutex("cond mutex", 0, 0, NULL);

    for (int kind = 0; kind < KIND_COUNT; kind++) {
        const char *name = kind_names[kind];
        const SceUID id = create_object(kind, "Lookup", SCE_KERNEL_ATTR_OPENABLE);
        PRINTF(test_created(id), "%s: create an openable object", name);
        const SceUID opened = open_object(kind, "Lookup");
        PRINTF(test_created(opened), "%s: open it by its name", name);
        PRINTF(opened == id, "%s: the opened UID is the object's own", name);
        if (opened >= 0)
            PRINTF(close_object(kind, opened), "%s: close it", name);
        print_open(kind, "lookup", "a name that differs in case");
        print_open(kind, "Look", "the start of the name");
        print_open(kind, "Lookup2", "the name with more after it");
        print_open(kind, "", "the empty name");
        print_open(kind, "Missing", "a name nothing has");
        const int other_kind = (kind + 1) % KIND_COUNT;
        const SceUID other = open_object(other_kind, "Lookup");
        PRINTF(test_created(other), "%s: open its name with the %s open call", name, kind_names[other_kind]);
        if (other >= 0)
            close_object(other_kind, other);
        const SceUID second = create_object(kind, "Lookup", SCE_KERNEL_ATTR_OPENABLE);
        PRINTF(test_created(second), "%s: create a second openable object with the name", name);
        if (second >= 0)
            delete_object(kind, second);
        PRINTF(delete_object(kind, id), "%s: delete the openable object", name);
        print_open(kind, "Lookup", "the name of the deleted object");

        // Objects that aren't openable
        const SceUID plain = create_object(kind, "Plain", 0);
        PRINTF(test_created(plain), "%s: create an object that isn't openable", name);
        print_open(kind, "Plain", "an object that isn't openable");
        const SceUID plain2 = create_object(kind, "Plain", 0);
        PRINTF(test_created(plain2), "%s: create a second one with the same name", name);
        const SceUID openable = create_object(kind, "Plain", SCE_KERNEL_ATTR_OPENABLE);
        PRINTF(test_created(openable), "%s: create an openable one with that name too", name);
        print_open(kind, "Plain", "the name the openable one shares");
        delete_object(kind, plain);
        delete_object(kind, plain2);
        delete_object(kind, openable);

        // The empty name on an openable object
        const SceUID empty = create_object(kind, "", SCE_KERNEL_ATTR_OPENABLE);
        PRINTF(test_created(empty), "%s: create an openable object with the empty name", name);
        print_open(kind, "", "the empty name of an openable object");
        if (empty >= 0)
            delete_object(kind, empty);
    }

    // The same openable name across types, with an openable semaphore holding it
    const SceUID shared = sceKernelCreateSema("Shared", SCE_KERNEL_ATTR_OPENABLE, 0, 1, NULL);
    for (int kind = EVENT_FLAG; kind < KIND_COUNT; kind++) {
        const SceUID other = create_object(kind, "Shared", SCE_KERNEL_ATTR_OPENABLE);
        PRINTF(test_created(other), "%s: create an openable object with the name of an openable semaphore", kind_names[kind]);
        if (other >= 0)
            delete_object(kind, other);
    }
    sceKernelDeleteSema(shared);

    // NULL last, so an implementation that crashes on it loses nothing else
    PRINT("simple event: open a NULL name", test_created(sceKernelOpenSimpleEvent(NULL)));

    sceKernelDeleteMutex(cond_mutex);
    test_exit();
    return 0;
}
