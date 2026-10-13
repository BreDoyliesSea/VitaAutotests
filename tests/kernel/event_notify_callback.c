// Callbacks registered to a simple event, notified by the set and pulse calls that notify
// callbacks: which callbacks they reach for each notify attribute, what the callback gets, and
// registering and unregistering in less common ways
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID event, cb;
static volatile int calls, got_notify_id, got_count, got_arg;

static int recording_callback(int notify_id, int count, int arg, void *common) {
    (void)common;
    calls++;
    got_notify_id = notify_id;
    got_count = count;
    got_arg = arg;
    return 0;
}

// Prints the callback's pending notifications, then runs it and prints what it got
static void print_notified(const char *name) {
    calls = 0;
    got_notify_id = got_count = got_arg = 0xDEAD;
    PRINTF(sceKernelGetCallbackCount(cb), "%s: the callback's notifications", name);
    sceKernelCheckCallback();
    PRINTF(calls, "%s: runs of the callback", name);
    if (calls) {
        PRINTF(got_notify_id == event, "%s: the notify ID is the event", name);
        PRINTF(got_count, "%s: the count", name);
        PRINTF(got_arg, "%s: the argument", name);
    }
}

// A thread with its own callback registered to the event, which waits on the event
static SceUID thread_cb;
static volatile int thread_calls, thread_wait_result, thread_calls_when_woken, thread_callbacks;

static int thread_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    thread_calls++;
    return 0;
}

static int waiting_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    thread_cb = sceKernelCreateCallback("thread", 0, thread_callback, NULL);
    sceKernelRegisterCallbackToEvent(event, thread_cb);
    SceUInt32 pattern;
    SceUInt64 user_data;
    SceUInt timeout = TEST_WAIT_US;
    if (thread_callbacks)
        thread_wait_result = sceKernelWaitEventCB(event, EVENT_BIT_A, &pattern, &user_data, &timeout);
    else
        thread_wait_result = sceKernelWaitEvent(event, EVENT_BIT_A, &pattern, &user_data, &timeout);
    thread_calls_when_woken = thread_calls;
    sceKernelCheckCallback();
    sceKernelDeleteCallback(thread_cb);
    return 0;
}

// Sets the event with notification while another thread waits on it, and prints which callbacks
// were notified
static void run_with_waiter(const char *name, SceUInt32 attr, int callbacks) {
    event = sceKernelCreateSimpleEvent("event", attr, 0, NULL);
    sceKernelRegisterCallbackToEvent(event, cb);
    thread_callbacks = callbacks;
    thread_calls = 0;
    thread_wait_result = thread_calls_when_woken = 0xDEAD;
    const SceUID thread = test_start_thread(waiting_thread);
    test_wait_until_waiting(thread);
    PRINTF(sceKernelSetEventWithNotifyCallback(event, EVENT_BIT_A, 0, 0x55), "%s: set with notification", name);
    test_join_thread(thread);
    PRINTF(thread_wait_result, "%s: the waiter's wait", name);
    PRINTF(thread_calls_when_woken, "%s: runs of the waiter's callback when its wait returned", name);
    PRINTF(thread_calls, "%s: runs of the waiter's callback after its next check", name);
    print_notified(name);
    sceKernelDeleteSimpleEvent(event);
    // Deleting the event notifies the callback, which the next case shouldn't count
    sceKernelCancelCallback(cb);
}

static int other_thread_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    return 0;
}

static int registering_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    thread_cb = sceKernelCreateCallback("other", 0, other_thread_callback, NULL);
    PRINT("another thread: register its callback", sceKernelRegisterCallbackToEvent(event, thread_cb));
    return 0;
}

int main(void) {
    cb = sceKernelCreateCallback("record", 0, recording_callback, NULL);

    // Notify all callbacks, with no waiter
    event = sceKernelCreateSimpleEvent("event", EVENT_ATTR_NOTIFY_CB_ALL, 0, NULL);
    PRINT("register", sceKernelRegisterCallbackToEvent(event, cb));
    PRINT("register the same callback again", sceKernelRegisterCallbackToEvent(event, cb));
    PRINT("set with notification", sceKernelSetEventWithNotifyCallback(event, EVENT_BIT_A, 0x1122, 0x33));
    SceUInt32 pattern = 0xDEAD;
    sceKernelGetEventPattern(event, &pattern);
    PRINT("the bits after it", pattern);
    print_notified("set with notification");
    PRINT("pulse with notification", sceKernelPulseEventWithNotifyCallback(event, EVENT_BIT_B, 0, 0x44));
    pattern = 0xDEAD;
    sceKernelGetEventPattern(event, &pattern);
    PRINT("the bits after it", pattern);
    print_notified("pulse with notification");
    sceKernelSetEventWithNotifyCallback(event, EVENT_BIT_A, 0, 1);
    sceKernelSetEventWithNotifyCallback(event, EVENT_BIT_B, 0, 2);
    print_notified("two sets with notification");
    PRINT("set without notification", sceKernelSetEvent(event, EVENT_BIT_C, 0));
    print_notified("set without notification");
    PRINT("set no bits with notification", sceKernelSetEventWithNotifyCallback(event, 0, 0, 5));
    print_notified("set no bits with notification");

    // Unregistering
    PRINT("unregister", sceKernelUnregisterCallbackFromEvent(event, cb));
    sceKernelSetEventWithNotifyCallback(event, EVENT_BIT_A, 0, 6);
    print_notified("set after unregistering");
    PRINT("unregister again", sceKernelUnregisterCallbackFromEvent(event, cb));
    sceKernelRegisterCallbackToEvent(event, cb);
    const SceUID registerer = test_start_thread(registering_thread);
    test_join_thread(registerer);
    sceKernelSetEventWithNotifyCallback(event, EVENT_BIT_A, 0, 7);
    PRINT("another thread's callback after the set", sceKernelGetCallbackCount(thread_cb));
    print_notified("set with another thread's callback registered too");
    PRINT("unregister all", sceKernelUnregisterCallbackFromEventAll(event));
    sceKernelSetEventWithNotifyCallback(event, EVENT_BIT_A, 0, 8);
    print_notified("set after unregistering all");
    PRINT("another thread's callback after it", sceKernelGetCallbackCount(thread_cb));
    sceKernelDeleteCallback(thread_cb);

    // A deleted callback that is still registered, and deleting the event
    const SceUID doomed = sceKernelCreateCallback("doomed", 0, recording_callback, NULL);
    sceKernelRegisterCallbackToEvent(event, doomed);
    sceKernelDeleteCallback(doomed);
    PRINT("set with a deleted callback registered", sceKernelSetEventWithNotifyCallback(event, EVENT_BIT_A, 0, 9));
    sceKernelRegisterCallbackToEvent(event, cb);
    PRINT("delete the event", sceKernelDeleteSimpleEvent(event));
    print_notified("deleting the event");
    PRINT("set a deleted event with notification", sceKernelSetEventWithNotifyCallback(event, EVENT_BIT_A, 0, 10));
    PRINT("register to a deleted event", sceKernelRegisterCallbackToEvent(event, cb));

    // Registering to what isn't an event
    const SceUID sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    PRINT("register to a semaphore", sceKernelRegisterCallbackToEvent(sema, cb));
    PRINT("register to an unknown UID", sceKernelRegisterCallbackToEvent(0x12345, cb));
    event = sceKernelCreateSimpleEvent("event", 0, 0, NULL);
    PRINT("register an unknown callback", sceKernelRegisterCallbackToEvent(event, 0x12345));
    PRINT("register a semaphore as the callback", sceKernelRegisterCallbackToEvent(event, sema));
    PRINT("unregister a callback that isn't registered", sceKernelUnregisterCallbackFromEvent(event, cb));
    PRINT("unregister all with none registered", sceKernelUnregisterCallbackFromEventAll(event));
    sceKernelDeleteSimpleEvent(event);

    // With a waiter, for each notify attribute and kind of wait
    run_with_waiter("notify all, a wait with callbacks", EVENT_ATTR_NOTIFY_CB_ALL, 1);
    run_with_waiter("notify all, a wait without callbacks", EVENT_ATTR_NOTIFY_CB_ALL, 0);
    run_with_waiter("notify the woken only, a wait with callbacks", EVENT_ATTR_NOTIFY_CB_WAKEUP_ONLY, 1);
    run_with_waiter("notify the woken only, a wait without callbacks", EVENT_ATTR_NOTIFY_CB_WAKEUP_ONLY, 0);

    // Notify the woken only, with no waiter
    event = sceKernelCreateSimpleEvent("event", EVENT_ATTR_NOTIFY_CB_WAKEUP_ONLY, 0, NULL);
    sceKernelRegisterCallbackToEvent(event, cb);
    sceKernelSetEventWithNotifyCallback(event, EVENT_BIT_A, 0, 11);
    print_notified("notify the woken only, no waiter");

    test_exit();
    return 0;
}
