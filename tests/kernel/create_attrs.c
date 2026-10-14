// Which attribute bits the create call of each sync object accepts. Every bit from 0 to 31 is
// passed alone. For each object type this prints the accepted bits as a mask, the error of the
// lowest rejected bit and whether every rejected bit gets that same error, the bits whose info
// reports the attribute just as given, and what creating with every accepted bit at once gives.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Kind { SEMA, EVENT_FLAG, MUTEX, LW_MUTEX, COND, LW_COND, RWLOCK, MSG_PIPE, SIMPLE_EVENT, TIMER, KIND_COUNT };
static const char *const kind_names[] = {
    "semaphore", "event flag", "mutex", "lightweight mutex", "condition variable", "lightweight condition variable",
    "reader/writer lock", "message pipe", "simple event", "timer",
};

static SceUID cond_mutex;
static SceKernelLwMutexWork lw_cond_mutex, lw_mutex;
static SceKernelLwCondWork lw_cond;

// Creates an object of the kind with attr and deletes it again. Returns 0 or the create error.
// When it was created, *reported gets the attribute its info reports and *deleted the delete result.
static int create(int kind, SceUInt32 attr, SceUInt32 *reported, int *deleted) {
    SceUID id = 0;
    int result = 0;
    *reported = 0xDEADBEEF;
    *deleted = 0;
    switch (kind) {
    case SEMA: {
        id = sceKernelCreateSema("attr sema", attr, 0, 1, NULL);
        if (id < 0)
            return id;
        SceKernelSemaInfo info = { .size = sizeof(info) };
        if (sceKernelGetSemaInfo(id, &info) == 0)
            *reported = info.attr;
        *deleted = sceKernelDeleteSema(id);
        return 0;
    }
    case EVENT_FLAG: {
        id = sceKernelCreateEventFlag("attr flag", attr, 0, NULL);
        if (id < 0)
            return id;
        SceKernelEventFlagInfo info = { .size = sizeof(info) };
        if (sceKernelGetEventFlagInfo(id, &info) == 0)
            *reported = info.attr;
        *deleted = sceKernelDeleteEventFlag(id);
        return 0;
    }
    case MUTEX: {
        id = sceKernelCreateMutex("attr mutex", attr, 0, NULL);
        if (id < 0)
            return id;
        MutexInfo info = { .size = sizeof(info) };
        if (sceKernelGetMutexInfo(id, (SceKernelMutexInfo *)&info) == 0)
            *reported = info.attr;
        *deleted = sceKernelDeleteMutex(id);
        return 0;
    }
    case LW_MUTEX: {
        result = sceKernelCreateLwMutex(&lw_mutex, "attr lw mutex", attr, 0, NULL);
        if (result < 0)
            return result;
        LwMutexInfo info = { .size = sizeof(info) };
        if (sceKernelGetLwMutexInfo(&lw_mutex, &info) == 0)
            *reported = info.attr;
        *deleted = sceKernelDeleteLwMutex(&lw_mutex);
        return result;
    }
    case COND: {
        id = sceKernelCreateCond("attr cond", attr, cond_mutex, NULL);
        if (id < 0)
            return id;
        SceKernelCondInfo info = { .size = sizeof(info) };
        if (sceKernelGetCondInfo(id, &info) == 0)
            *reported = info.attr;
        *deleted = sceKernelDeleteCond(id);
        return 0;
    }
    case LW_COND: {
        result = sceKernelCreateLwCond(&lw_cond, "attr lw cond", attr, &lw_cond_mutex, NULL);
        if (result < 0)
            return result;
        LwCondInfo info = { .size = sizeof(info) };
        if (sceKernelGetLwCondInfo(&lw_cond, &info) == 0)
            *reported = info.attr;
        *deleted = sceKernelDeleteLwCond(&lw_cond);
        return result;
    }
    case RWLOCK: {
        id = sceKernelCreateRWLock("attr rwlock", attr, NULL);
        if (id < 0)
            return id;
        SceKernelRWLockInfo info = { .size = sizeof(info) };
        if (sceKernelGetRWLockInfo(id, &info) == 0)
            *reported = info.attr;
        *deleted = sceKernelDeleteRWLock(id);
        return 0;
    }
    case MSG_PIPE: {
        id = sceKernelCreateMsgPipe("attr pipe", MSG_PIPE_TYPE_USER, attr, 16, NULL);
        if (id < 0)
            return id;
        SceKernelMppInfo info = { .size = sizeof(info) };
        if (sceKernelGetMsgPipeInfo(id, &info) == 0)
            *reported = info.attr;
        *deleted = sceKernelDeleteMsgPipe(id);
        return 0;
    }
    case SIMPLE_EVENT: {
        id = sceKernelCreateSimpleEvent("attr event", attr, 0, NULL);
        if (id < 0)
            return id;
        EventInfo info = { .size = sizeof(info) };
        if (sceKernelGetEventInfo(id, &info) == 0)
            *reported = info.attr;
        *deleted = sceKernelDeleteSimpleEvent(id);
        return 0;
    }
    case TIMER: {
        id = sceKernelCreateTimer("attr timer", attr, NULL);
        if (id < 0)
            return id;
        TimerInfo info = { .size = sizeof(info) };
        if (sceKernelGetTimerInfo(id, &info) == 0)
            *reported = info.attr;
        *deleted = sceKernelDeleteTimer(id);
        return 0;
    }
    }
    return result;
}

int main(void) {
    cond_mutex = sceKernelCreateMutex("cond mutex", 0, 0, NULL);
    sceKernelCreateLwMutex(&lw_cond_mutex, "lw cond mutex", 0, 0, NULL);

    for (int kind = 0; kind < KIND_COUNT; kind++) {
        const char *name = kind_names[kind];
        SceUInt32 accepted = 0, as_given = 0, reported;
        int first_error = 0, same_error = 1, all_deleted = 1, deleted;
        for (int bit = 0; bit < 32; bit++) {
            const SceUInt32 attr = 1u << bit;
            const int result = create(kind, attr, &reported, &deleted);
            if (result == 0) {
                accepted |= attr;
                if (reported == attr)
                    as_given |= attr;
                if (deleted != 0)
                    all_deleted = 0;
            } else if (first_error == 0) {
                first_error = result;
            } else if (result != first_error) {
                same_error = 0;
            }
        }
        PRINTF(accepted, "%s: the accepted bits", name);
        PRINTF(first_error, "%s: the error of the lowest rejected bit", name);
        PRINTF(same_error, "%s: every rejected bit gets that error", name);
        PRINTF(as_given, "%s: the bits its info reports as given", name);
        PRINTF(all_deleted, "%s: every one created deletes", name);
        const int together = create(kind, accepted, &reported, &deleted);
        PRINTF(together, "%s: create with every accepted bit", name);
        PRINTF(together == 0 ? reported : 0, "%s: the attribute its info reports for them", name);
    }

    sceKernelDeleteLwMutex(&lw_cond_mutex);
    sceKernelDeleteMutex(cond_mutex);
    test_exit();
    return 0;
}
