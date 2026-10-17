// Which UID the kernel reports back for an object it was given through a UID opened from it, and
// what such a reference keeps once that UID or the creating one goes. The cases:
// - a condition variable made with a UID opened from an openable mutex: the mutex UID its info
//   reports, and a wait on it with the mutex locked through the creating UID, which times out
//   after 1 ms. Then the same once the opened mutex UID is closed, and the info and a signal once
//   the mutex's creating UID is deleted too.
// - a callback registered to an openable simple event through an opened UID, notified by a set
//   with notification through the creating UID, and then registered through the creating UID and
//   notified through the opened one: the notifications, the runs, and the notify ID it gets.
//   Unregistering through the UID it wasn't registered through. Then with the callback registered
//   through the opened UID, the creating UID deleted and a set through the opened UID, and the
//   opened UID closed, the event's last UID, with the notifications and the runs after each.
// - a wait on several events with an opened UID of a set simple event: the event UID in its
//   result. Last, the same event given twice, through the creating UID and an opened one, and the
//   UID and result of each result the wait gives.
// A reported UID prints 0 for the UID the object was given by, 1 for its other UID and 2 for
// anything else. For the event given twice, 0 stands for the creating UID and 1 for the opened
// one.
//
// On a PS Vita TV on firmware 3.65 a condition variable made with an opened mutex UID reports the
// mutex's creating UID, and a wait with the mutex locked through that UID times out with 0x80028005
// (WAIT_TIMEOUT), also after the opened UID is closed. Once the creating UID is deleted too, the
// info call works but reports neither UID, and a signal and the delete work. A callback registered
// through either UID is notified by a set through the other and gets the UID it was registered
// through as its notify ID. Unregistering through the other UID works, after which one through that
// UID gives 0x800280A3 (CALLBACK_NOT_REGISTERED). Registered through the opened UID, it is notified
// once each by the delete of the creating UID, a set after it and the close of the last UID, with
// the opened UID. A wait on several events reports an event given by its opened UID by that UID,
// but given through both UIDs it gives two results that both report the creating UID.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static volatile int calls, got_notify_id;

static int recording_callback(int notify_id, int count, int arg, void *common) {
    (void)count;
    (void)arg;
    (void)common;
    calls++;
    got_notify_id = notify_id;
    return 0;
}

// 0 for given, 1 for other, 2 for anything else
static int which(SceUID seen, SceUID given, SceUID other) {
    return seen == given ? 0 : seen == other ? 1 : 2;
}

static int cond_mutex_id(SceUID cond, int *result) {
    SceKernelCondInfo info = { .size = sizeof(info) };
    *result = sceKernelGetCondInfo(cond, &info);
    return info.mutexId;
}

// A wait on cond with mutex locked, which nothing signals
static int timed_cond_wait(SceUID cond, SceUID mutex) {
    sceKernelLockMutex(mutex, 1, NULL);
    SceUInt timeout = 1000;
    const int result = sceKernelWaitCond(cond, &timeout);
    sceKernelUnlockMutex(mutex, 1);
    return result;
}

static void cond_cases(void) {
    const SceUID mutex = sceKernelCreateMutex("Cond mutex", SCE_KERNEL_ATTR_OPENABLE, 0, NULL);
    const SceUID opened = sceKernelOpenMutex("Cond mutex");
    PRINT("condition variable: create the mutex and open it",
        mutex < 0 ? mutex : test_created(opened));
    const SceUID cond = sceKernelCreateCond("cond", 0, opened, NULL);
    PRINT("condition variable: create it with the opened mutex UID", test_created(cond));
    int result;
    SceUID seen = cond_mutex_id(cond, &result);
    PRINT("condition variable: its info", result);
    PRINT("condition variable: the mutex UID it reports", which(seen, opened, mutex));
    PRINT("condition variable: a wait with the mutex locked through the creating UID",
        timed_cond_wait(cond, mutex));

    PRINT("condition variable: close the opened mutex UID", sceKernelCloseMutex(opened));
    seen = cond_mutex_id(cond, &result);
    PRINT("condition variable: its info after", result);
    PRINT("condition variable: the mutex UID it reports after", which(seen, opened, mutex));
    PRINT("condition variable: a wait after", timed_cond_wait(cond, mutex));

    PRINT("condition variable: delete the mutex's creating UID", sceKernelDeleteMutex(mutex));
    seen = cond_mutex_id(cond, &result);
    PRINT("condition variable: its info after the delete", result);
    PRINT("condition variable: the mutex UID it reports after the delete",
        which(seen, opened, mutex));
    PRINT("condition variable: a signal after the delete", sceKernelSignalCond(cond));
    PRINT("condition variable: delete it", sceKernelDeleteCond(cond));
}

// Prints the callback's notifications, runs it, and prints what it got
static void check(const char *name, SceUID cb, SceUID given, SceUID other) {
    calls = 0;
    got_notify_id = 0xDEAD;
    PRINTF(sceKernelGetCallbackCount(cb), "%s: the callback's notifications", name);
    sceKernelCheckCallback();
    PRINTF(calls, "%s: runs of the callback", name);
    if (calls)
        PRINTF(which(got_notify_id, given, other), "%s: the notify ID it got", name);
}

static void callback_cases(void) {
    const SceUID cb = sceKernelCreateCallback("record", 0, recording_callback, NULL);
    const SceUID event = sceKernelCreateSimpleEvent("Notified", SCE_KERNEL_ATTR_OPENABLE, 0, NULL);
    const SceUID opened = sceKernelOpenSimpleEvent("Notified");
    PRINT("callback: create the event and open it", event < 0 ? event : test_created(opened));

    PRINT("callback: register through the opened UID",
        sceKernelRegisterCallbackToEvent(opened, cb));
    PRINT("callback: set with notification through the creating UID",
        sceKernelSetEventWithNotifyCallback(event, EVENT_BIT_A, 0, 0x33));
    check("callback: registered through the opened UID", cb, opened, event);
    PRINT("callback: unregister through the creating UID",
        sceKernelUnregisterCallbackFromEvent(event, cb));
    PRINT("callback: unregister through the opened UID",
        sceKernelUnregisterCallbackFromEvent(opened, cb));

    PRINT("callback: register through the creating UID",
        sceKernelRegisterCallbackToEvent(event, cb));
    PRINT("callback: set with notification through the opened UID",
        sceKernelSetEventWithNotifyCallback(opened, EVENT_BIT_B, 0, 0x44));
    check("callback: registered through the creating UID", cb, event, opened);
    sceKernelUnregisterCallbackFromEvent(event, cb);

    sceKernelRegisterCallbackToEvent(opened, cb);
    PRINT("callback: delete the creating UID", sceKernelDeleteSimpleEvent(event));
    check("callback: after the delete", cb, opened, event);
    PRINT("callback: set with notification through the opened UID after it",
        sceKernelSetEventWithNotifyCallback(opened, EVENT_BIT_C, 0, 0x55));
    check("callback: the set after the delete", cb, opened, event);
    PRINT("callback: close the opened UID, the last", sceKernelCloseSimpleEvent(opened));
    check("callback: after the close", cb, opened, event);
    sceKernelDeleteCallback(cb);
}

static void multiple_event_cases(void) {
    const SceUID event = sceKernelCreateSimpleEvent("Multiple", SCE_KERNEL_ATTR_OPENABLE, 0, NULL);
    const SceUID opened = sceKernelOpenSimpleEvent("Multiple");
    PRINT("several events: create the event and open it",
        event < 0 ? event : test_created(opened));
    const SceUID other = sceKernelCreateSimpleEvent("other", 0, 0, NULL);
    sceKernelSetEvent(event, EVENT_BIT_A, 0);
    WaitEvent events[2] = { { other, EVENT_BIT_A }, { opened, EVENT_BIT_A } };
    ResultEvent results[2] = { { 0 } };
    SceUInt32 timeout = 1000;
    PRINT("several events: wait with the set one given by its opened UID",
        sceKernelWaitMultipleEvents(events, 2, EVENT_WAIT_MODE_OR, results, &timeout));
    PRINT("several events: the event UID in the result",
        which(results[0].eventId, opened, event));
    PRINT("several events: its result", results[0].result);

    // The same event twice last, in case an implementation can't take it
    WaitEvent same[2] = { { event, EVENT_BIT_A }, { opened, EVENT_BIT_A } };
    ResultEvent same_results[2] = { { 0 } };
    timeout = 1000;
    const int count =
        sceKernelWaitMultipleEvents(same, 2, EVENT_WAIT_MODE_OR, same_results, &timeout);
    PRINT("several events: wait on the event through both UIDs", count);
    for (int i = 0; i < count && i < 2; i++) {
        const int uid = which(same_results[i].eventId, event, opened);
        PRINTF(uid, "several events: result %d: its UID", i);
        PRINTF(same_results[i].result, "several events: result %d: its result", i);
    }
    sceKernelCloseSimpleEvent(opened);
    sceKernelDeleteSimpleEvent(event);
    sceKernelDeleteSimpleEvent(other);
}

int main(void) {
    cond_cases();
    callback_cases();
    multiple_event_cases();
    test_exit();
    return 0;
}
