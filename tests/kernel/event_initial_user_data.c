// The user data a simple event and a timer start with. Every case reads it back as two
// comparisons, whether its high word is the UID of the thread that created the object and whether
// its low word is the process ID, and never prints the words themselves. Each out value starts as
// a marker that matches neither, so a call that doesn't write it shows. The cases:
// - a simple event created by the main thread, with the create bit 0x4 polled for, which prints
//   the poll's result, the bits it got and the user data it got. Then the event's info, which
//   prints the result, the pattern and the user data
// - the same for a simple event created by another thread, which stays alive until the main
//   thread has read the event
// - a timer created by the main thread, where the info prints the same
// - the timer with a one shot set event of 50 ms, started, and a wait for the timer's event with
//   a timeout of 2 seconds, which prints the wait's result, the bits and the user data it got,
//   and the user data of the info after the wait
//
// On a PS Vita TV on firmware 3.65 a simple event and a timer both start with the create bit 0x4
// set and with user data that holds the creating thread's UID in its high word and the process ID
// in its low word, the same words an open or a close of a simple event writes. The poll gets the
// bit 0x4 and that user data, and so does the info. A timer's event sets the bit 0x8000 with
// user data 0, so the wait gets 0 and the info reports a user data of 0 after it.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define CREATE_BIT 0x4
#define MARKER 0xFFFFFFFFFFFFFFFFull
#define TIMER_INTERVAL_US 50000
// Forty times the timer's interval, so a wait that gets the event never times out first
#define TIMER_WAIT_US 2000000

static SceUID process, gate;
static volatile SceUID other_thread, other_event;
static volatile int other_ready;

static void print_user_data(const char *name, const char *what, SceUInt64 user_data, SceUID creator) {
    PRINTF((SceUInt32)(user_data >> 32) == (SceUInt32)creator,
        "%s: the high word of %s is the creating thread's UID", name, what);
    PRINTF((SceUInt32)user_data == (SceUInt32)process,
        "%s: the low word of %s is the process ID", name, what);
}

static void print_info(const char *name, SceUID id, SceUID creator) {
    EventInfo info = { .size = sizeof(info) };
    info.userData = MARKER;
    PRINTF(sceKernelGetEventInfo(id, &info), "%s: info", name);
    PRINTF(info.eventPattern & CREATE_BIT, "%s: the info's pattern has the create bit", name);
    print_user_data(name, "the info's user data", info.userData, creator);
}

// A poll for the create bit, which prints what it got
static void print_poll(const char *name, SceUID id, SceUID creator) {
    SceUInt32 result = 0xDEAD;
    SceUInt64 user_data = MARKER;
    PRINTF(sceKernelPollEvent(id, CREATE_BIT, &result, &user_data), "%s: poll for the create bit", name);
    PRINTF(result, "%s: the bits the poll got", name);
    print_user_data(name, "the poll's user data", user_data, creator);
}

static int creator_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    other_thread = sceKernelGetThreadId();
    other_event = sceKernelCreateSimpleEvent("other", EVENT_ATTR_MANUAL_RESET, 0, NULL);
    __atomic_store_n(&other_ready, 1, __ATOMIC_RELEASE);
    SceUInt32 timeout = 3 * TEST_WAIT_US;
    sceKernelWaitSema(gate, 1, &timeout);
    return 0;
}

int main(void) {
    process = sceKernelGetProcessId();

    // A simple event created by the main thread
    const SceUID main_thread = sceKernelGetThreadId();
    SceUID event = sceKernelCreateSimpleEvent("main", EVENT_ATTR_MANUAL_RESET, 0, NULL);
    PRINTF(test_created(event), "main thread's simple event: create");
    print_poll("main thread's simple event", event, main_thread);
    print_info("main thread's simple event", event, main_thread);
    sceKernelDeleteSimpleEvent(event);

    // A simple event created by another thread, which waits for the gate until the main thread
    // has read it
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);
    other_thread = 0;
    other_event = -1;
    const SceUID creator = test_start_thread(creator_thread);
    TEST_WAIT_FOR(__atomic_load_n(&other_ready, __ATOMIC_ACQUIRE));
    PRINTF(test_created(other_event), "other thread's simple event: create");
    print_poll("other thread's simple event", other_event, other_thread);
    print_info("other thread's simple event", other_event, other_thread);
    sceKernelDeleteSimpleEvent(other_event);
    sceKernelSignalSema(gate, 1);
    test_join_thread(creator);
    sceKernelDeleteSema(gate);

    // A timer created by the main thread, and the user data its event fires with
    const SceUID timer = sceKernelCreateTimer("timer", EVENT_ATTR_MANUAL_RESET, NULL);
    PRINTF(test_created(timer), "timer: create");
    print_info("timer", timer, main_thread);
    SceUInt64 interval = TIMER_INTERVAL_US;
    PRINTF(sceKernelSetTimerEvent(timer, TIMER_TYPE_SET_EVENT, &interval, 0), "timer: set a one shot set event");
    PRINTF(sceKernelStartTimer(timer), "timer: start");
    SceUInt32 result = 0xDEAD;
    SceUInt64 user_data = MARKER;
    SceUInt32 timeout = TIMER_WAIT_US;
    PRINTF(sceKernelWaitEvent(timer, EVENT_TIMER, &result, &user_data, &timeout), "timer: wait for its event");
    PRINTF(result, "timer: the bits the wait got");
    PRINTF(user_data == 0, "timer: the user data the wait got is 0");
    EventInfo info = { .size = sizeof(info) };
    info.userData = MARKER;
    PRINTF(sceKernelGetEventInfo(timer, &info), "timer: info after the wait");
    PRINTF(info.userData == 0, "timer: the info's user data after the wait is 0");
    sceKernelStopTimer(timer);
    sceKernelDeleteTimer(timer);

    test_exit();
    return 0;
}
