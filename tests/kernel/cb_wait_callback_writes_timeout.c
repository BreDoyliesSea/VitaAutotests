// What a callback that writes the caller's timeout variable during a wait with callbacks changes. A
// thread T creates a callback C, notifies it, which only leaves it pending since T isn't waiting,
// and waits with callbacks with a timeout in a variable. C runs at the start of the wait and writes
// that variable. Every case has fresh objects and runs T alone, the main thread waits for T to end.
// Two sets of cases:
// - C writes 12345 and gives T's object what T waits for, with a timeout of 1 s, so the take after
//   the callbacks succeeds at once and the wait never sleeps. These print what the variable holds
//   after the wait: the 1000000 passed, the 12345 C wrote, or something else.
// - C writes 1 and gives nothing, with a timeout of 0.5 s, so the wait times out. These print
//   whether it lasted at least 0.25 s, which it does if C's write didn't shorten the timeout.
// Each wait takes what it waits for:
// - semaphore: sceKernelWaitSemaCB for 1, which C signals
// - event flag: sceKernelWaitEventFlagCB for bit 0x1, which C sets
// - simple event: sceKernelWaitEventCB for a user bit, which C sets
// - message pipe: sceKernelReceiveMsgPipeCB of 4 bytes in FULL mode, which C sends to the same pipe
// Every case also prints the wait's result and how often C ran.
//
// On a PS Vita TV on firmware 3.65 the waits C gives return 0. The semaphore, event flag and simple
// event waits leave the 1000000 passed in the variable, and the message pipe wait leaves C's 12345.
// The waits C gives nothing return 0x80028005 and last at least 0.25 s. C runs once in each case.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define WAIT_US 1000000
#define WRITTEN 12345
#define SHORT_WAIT_US 500000
#define SHORT_WRITTEN 1
#define LASTED_US 250000
#define MESSAGE_SIZE 4

typedef enum Kind { SEMA, FLAG, EVENT, PIPE } Kind;

static Kind kind;
static int gives;
static SceUID object;
static volatile int callback_runs, wait_result;
static SceUInt *volatile timeout_variable;
static volatile SceUInt time_after;
static volatile SceUInt64 elapsed;

// Gives T's wait what it waits for
static void give(void) {
    SceUInt timeout = TEST_WAIT_US;
    switch (kind) {
    case SEMA: sceKernelSignalSema(object, 1); break;
    case FLAG: sceKernelSetEventFlag(object, 0x1); break;
    case EVENT: sceKernelSetEvent(object, EVENT_BIT_A, 0); break;
    case PIPE: sceKernelSendMsgPipe(object, "ABCD", MESSAGE_SIZE, MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    }
}

// Writes the caller's timeout variable, then gives the wait what it waits for if the case says so
static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    if (gives) {
        *timeout_variable = WRITTEN;
        give();
    } else {
        *timeout_variable = SHORT_WRITTEN;
    }
    return 0;
}

// Creates a callback, notifies it, and waits with callbacks
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    const SceUID callback = sceKernelCreateCallback("writer", 0, callback_entry, NULL);
    sceKernelNotifyCallback(callback, 1);
    SceUInt timeout = gives ? WAIT_US : SHORT_WAIT_US;
    timeout_variable = &timeout;
    SceUInt32 bits = 0;
    SceUInt64 user_data = 0;
    char buffer[MESSAGE_SIZE];
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    switch (kind) {
    case SEMA: wait_result = sceKernelWaitSemaCB(object, 1, &timeout); break;
    case FLAG: wait_result = sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout); break;
    case EVENT: wait_result = sceKernelWaitEventCB(object, EVENT_BIT_A, &bits, &user_data, &timeout); break;
    case PIPE: wait_result = sceKernelReceiveMsgPipeCB(object, buffer, sizeof(buffer), MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    }
    elapsed = sceKernelGetProcessTimeWide() - start;
    time_after = timeout;
    sceKernelDeleteCallback(callback);
    return 0;
}

static void run_case(const char *name, Kind k, int give_it) {
    kind = k;
    gives = give_it;
    callback_runs = 0;
    wait_result = NOT_DONE;
    time_after = NOT_DONE;
    switch (k) {
    case SEMA: object = sceKernelCreateSema("sema", 0, 0, 1, NULL); break;
    case FLAG: object = sceKernelCreateEventFlag("flag", 0, 0, NULL); break;
    case EVENT: object = sceKernelCreateSimpleEvent("event", 0, 0, NULL); break;
    case PIPE: object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL); break;
    }

    test_join_thread(test_start_thread(t_entry));

    PRINTF(wait_result, "%s: the wait", name);
    PRINTF(callback_runs, "%s: runs of the callback", name);
    if (give_it) {
        PRINTF(time_after == WAIT_US, "%s: the timeout holds the value passed", name);
        PRINTF(time_after == WRITTEN, "%s: the timeout holds the callback's value", name);
    } else {
        PRINTF(elapsed >= LASTED_US, "%s: it lasted at least 0.25 s", name);
    }

    switch (k) {
    case SEMA: sceKernelDeleteSema(object); break;
    case FLAG: sceKernelDeleteEventFlag(object); break;
    case EVENT: sceKernelDeleteSimpleEvent(object); break;
    case PIPE: sceKernelDeleteMsgPipe(object); break;
    }
}

int main(void) {
    run_case("semaphore, the callback gives", SEMA, 1);
    run_case("event flag, the callback gives", FLAG, 1);
    run_case("simple event, the callback gives", EVENT, 1);
    // The callback sends to the pipe, so a wait that holds the pipe while it runs callbacks hangs
    run_case("message pipe, the callback gives", PIPE, 1);

    run_case("semaphore, the callback gives nothing", SEMA, 0);
    run_case("event flag, the callback gives nothing", FLAG, 0);
    run_case("simple event, the callback gives nothing", EVENT, 0);
    run_case("message pipe, the callback gives nothing", PIPE, 0);

    test_exit();
    return 0;
}
