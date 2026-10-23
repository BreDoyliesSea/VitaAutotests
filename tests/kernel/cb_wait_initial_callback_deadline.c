// Whether the run of a callback pending when a wait with callbacks starts counts against the
// wait's timeout. A thread T creates a callback C whose run delays T for 1 s, notifies it, which
// only leaves it pending since T isn't waiting, and waits with callbacks with a timeout of 0.5 s on
// an object nothing gives. C runs at the start of the wait. A wait that built its deadline before
// running C finds it gone once C returned and times out right away, about 1 s after it started. One
// that built it after C waits the whole timeout too, about 1.5 s. Every case has fresh objects and
// runs T alone, the main thread waits for T to end. The waits:
// - semaphore: sceKernelWaitSemaCB for 1
// - event flag: sceKernelWaitEventFlagCB for bit 0x1
// - simple event: sceKernelWaitEventCB for a user bit
// - message pipe: sceKernelReceiveMsgPipeCB of 4 bytes in FULL mode
// Each case prints the wait's result, the time left in the timeout variable, how often C ran, and
// whether the wait returned less than 1.25 s after it started.
//
// On a PS Vita TV on firmware 3.65 all four waits return 0x80028005 with the time left 0 less than
// 1.25 s after they started, and C runs once in each.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define CALLBACK_US 1000000
#define WAIT_US 500000
#define LIMIT_US 1250000
#define MESSAGE_SIZE 4

typedef enum Kind { SEMA, FLAG, EVENT, PIPE } Kind;

static Kind kind;
static SceUID object;
static volatile int callback_runs, wait_result;
static volatile SceUInt time_after;
static volatile SceUInt64 elapsed;

// Keeps T busy for longer than its wait's timeout
static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    sceKernelDelayThread(CALLBACK_US);
    return 0;
}

// Creates a callback, notifies it, and waits with callbacks on an object nothing gives
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    const SceUID callback = sceKernelCreateCallback("long", 0, callback_entry, NULL);
    sceKernelNotifyCallback(callback, 1);
    SceUInt timeout = WAIT_US;
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

static void run_case(const char *name, Kind k) {
    kind = k;
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
    PRINTF(time_after, "%s: the time left", name);
    PRINTF(callback_runs, "%s: runs of the callback", name);
    PRINTF(elapsed < LIMIT_US, "%s: it returned less than 1.25 s after it started", name);

    switch (k) {
    case SEMA: sceKernelDeleteSema(object); break;
    case FLAG: sceKernelDeleteEventFlag(object); break;
    case EVENT: sceKernelDeleteSimpleEvent(object); break;
    case PIPE: sceKernelDeleteMsgPipe(object); break;
    }
}

int main(void) {
    run_case("semaphore", SEMA);
    run_case("event flag", FLAG);
    run_case("simple event", EVENT);
    run_case("message pipe", PIPE);

    test_exit();
    return 0;
}
