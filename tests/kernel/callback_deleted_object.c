// What a wait with callbacks does when its object is deleted while the thread runs a callback, out
// of the object's queue. Thread T waits with callbacks on an object for 4 s. The main thread
// notifies T's callback once T waits. The callback waits on a gate semaphore, with a finite
// timeout, so T is out of the object's queue until the main thread lets it go. The main thread
// waits until T's callback blocks on the gate, deletes T's object, records the process time and
// signals the gate. T records its wait's result and the process time when its wait returned. Every
// case has fresh objects and a fresh T, and the main thread prints after T ended. A T whose wait
// goes on after the delete returns at its timeout, 4 s after it started, so the time between the
// signal and T's return tells the two outcomes apart, a few ms against about 4 s. The cases:
// - a semaphore at 0
// - an event flag without SCE_EVENT_WAITMULTIPLE, which T waits on for bit 0x1
// - a lightweight mutex that the main thread locked
// - a simple event, which T waits on for a user bit
// - a mutex that the main thread locked, and deletes while it holds it
// Each case prints whether T's callback waits on the gate when the object is deleted, the delete's
// result, T's wait result and whether T's wait returned within 1 s of the gate signal.
//
// On a PS Vita TV on firmware 3.65 the delete succeeds in every case, and T's wait returns
// 0x80028006 right after the signal, with no wait for the timeout. The wait does not go on with
// the deleted object.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define WAIT_US 4000000
#define GATE_TIMEOUT_US 6000000
#define SOON_US 1000000

typedef enum Kind { SEMA, FLAG, LW_MUTEX, SIMPLE_EVENT, MUTEX } Kind;

static Kind kind;
static SceUID object, gate;
static SceKernelLwMutexWork lw_mutex;
static volatile SceUID callback;
static volatile int callback_runs, t_result;
static volatile SceUInt64 t_end;

// Blocks on the gate semaphore until the main thread signals it
static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    SceUInt timeout = GATE_TIMEOUT_US;
    sceKernelWaitSema(gate, 1, &timeout);
    return 0;
}

// Creates the callback, waits with callbacks on the object of the case and records the end
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = WAIT_US;
    SceUInt32 bits;
    SceUInt64 user_data;
    callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
    switch (kind) {
    case SEMA: t_result = sceKernelWaitSemaCB(object, 1, &timeout); break;
    case FLAG: t_result = sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout); break;
    case LW_MUTEX: t_result = sceKernelLockLwMutexCB(&lw_mutex, 1, &timeout); break;
    case SIMPLE_EVENT: t_result = sceKernelWaitEventCB(object, EVENT_BIT_A, &bits, &user_data, &timeout); break;
    case MUTEX: t_result = sceKernelLockMutexCB(object, 1, &timeout); break;
    }
    t_end = sceKernelGetProcessTimeWide();
    sceKernelDeleteCallback(callback);
    return 0;
}

// Creates the object of the case, locked by the main thread where it is a lock
static void create_object(void) {
    switch (kind) {
    case SEMA: object = sceKernelCreateSema("sema", 0, 0, 1, NULL); break;
    case FLAG: object = sceKernelCreateEventFlag("flag", 0, 0, NULL); break;
    case LW_MUTEX:
        sceKernelCreateLwMutex(&lw_mutex, "lw mutex", 0, 0, NULL);
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        break;
    case SIMPLE_EVENT: object = sceKernelCreateSimpleEvent("event", 0, 0, NULL); break;
    case MUTEX:
        object = sceKernelCreateMutex("mutex", 0, 0, NULL);
        sceKernelLockMutex(object, 1, NULL);
        break;
    }
}

static int delete_object(void) {
    switch (kind) {
    case SEMA: return sceKernelDeleteSema(object);
    case FLAG: return sceKernelDeleteEventFlag(object);
    case LW_MUTEX: return sceKernelDeleteLwMutex(&lw_mutex);
    case SIMPLE_EVENT: return sceKernelDeleteSimpleEvent(object);
    case MUTEX: return sceKernelDeleteMutex(object);
    }
    return NOT_DONE;
}

// Returns the wait type of a wait with callbacks on the object of the case
static SceUInt32 wait_type(void) {
    switch (kind) {
    case SEMA: return WAIT_TYPE_SEMA | WAIT_TYPE_CB;
    case FLAG: return WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB;
    case LW_MUTEX: return WAIT_TYPE_LW_MUTEX | WAIT_TYPE_CB;
    case SIMPLE_EVENT: return WAIT_TYPE_EVENT | WAIT_TYPE_CB;
    case MUTEX: return WAIT_TYPE_MUTEX | WAIT_TYPE_CB;
    }
    return 0;
}

// Returns whether thread waits with callbacks on the object of the case. A lightweight mutex has
// no UID that the test knows, so only its wait type counts.
static int waits_on_object(SceUID thread) {
    if (kind == LW_MUTEX) {
        SceKernelThreadInfo info = { .size = sizeof(info) };
        return sceKernelGetThreadInfo(thread, &info) == 0 && info.waitType == wait_type();
    }
    return test_in_wait(thread, wait_type(), object);
}

static void run_case(const char *name, Kind k) {
    kind = k;
    callback = 0;
    callback_runs = 0;
    t_result = NOT_DONE;
    t_end = 0;
    create_object();
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);

    const SceUID t = test_start_thread(t_entry);
    TEST_WAIT_FOR(waits_on_object(t));
    sceKernelNotifyCallback(callback, 1);
    TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, gate));
    const int in_gate = test_in_wait(t, WAIT_TYPE_SEMA, gate);
    const int delete_result = delete_object();
    const SceUInt64 gate_time = sceKernelGetProcessTimeWide();
    sceKernelSignalSema(gate, 1);
    test_join_thread(t);
    const SceInt64 elapsed = (SceInt64)(t_end - gate_time);
    sceKernelDeleteSema(gate);

    PRINTF(in_gate, "%s: T's callback waits on the gate when the object is deleted", name);
    PRINTF(delete_result, "%s: the delete", name);
    PRINTF(t_result, "%s: T's wait", name);
    PRINTF(elapsed < SOON_US, "%s: T's wait returned within 1 s of the gate signal", name);
}

int main(void) {
    run_case("semaphore", SEMA);
    run_case("event flag", FLAG);
    run_case("lightweight mutex", LW_MUTEX);
    run_case("simple event", SIMPLE_EVENT);
    run_case("mutex", MUTEX);

    test_exit();
    return 0;
}
