// What a wait with callbacks returns when its deadline is already gone and a callback is pending
// at the start that notifies itself again. A thread T creates a callback C and notifies it, which
// only leaves it pending since T isn't waiting. Then T waits with callbacks with a timeout of 0. C
// runs at the start of the wait, and its first run notifies C again, so a callback is pending once
// more after the take failed. Its second run, whenever it comes, gives T's object what T waits for.
// Every case has fresh objects and runs T alone, the main thread waits for T to end and then checks
// whether the object holds what C's second run gave. Each wait takes what it waits for:
// - semaphore: sceKernelWaitSemaCB for 1, which C's second run signals
// - event flag: sceKernelWaitEventFlagCB for bit 0x1, which C's second run sets
// - simple event: sceKernelWaitEventCB for a user bit, which C's second run sets
// - message pipe: sceKernelReceiveMsgPipeCB of 4 bytes in FULL mode, which C's second run sends to
//   the same pipe
// Each case prints the wait's result, the time left in the timeout variable, how often C ran, and
// whether the object holds what C's second run gave, with a poll after T ended. The event flag case
// also prints the wait's out bits, which start as 0xDEAD.
//
// On a PS Vita TV on firmware 3.65 all four waits return 0x80028005 with the time left 0, and the
// message pipe wait received nothing. C runs twice in each: at the start of the wait, and once more
// before the wait returns, so the object holds what the second run gave once T ended. The event
// flag wait's out bits read 0, the flag's bits when its wait ended, not the bit C's second run set.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define MESSAGE_SIZE 4

typedef enum Kind { SEMA, FLAG, EVENT, PIPE } Kind;

static Kind kind;
static SceUID object;
static volatile SceUID callback;
static volatile int callback_runs, wait_result;
static volatile SceUInt time_after;
static volatile SceUInt32 bits_after;
static volatile SceSize moved;

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

// The first run notifies the callback again and the second gives T's wait what it waits for
static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    if (callback_runs == 1)
        sceKernelNotifyCallback(callback, 1);
    else if (callback_runs == 2)
        give();
    return 0;
}

// Creates a callback, notifies it, and waits with callbacks with a timeout of 0
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("zero", 0, callback_entry, NULL);
    sceKernelNotifyCallback(callback, 1);
    SceUInt timeout = 0;
    SceUInt32 bits = NOT_DONE;
    SceUInt64 user_data = 0;
    SceSize size = NOT_DONE;
    char buffer[MESSAGE_SIZE];
    switch (kind) {
    case SEMA: wait_result = sceKernelWaitSemaCB(object, 1, &timeout); break;
    case FLAG: wait_result = sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout); break;
    case EVENT: wait_result = sceKernelWaitEventCB(object, EVENT_BIT_A, &bits, &user_data, &timeout); break;
    case PIPE: wait_result = sceKernelReceiveMsgPipeCB(object, buffer, sizeof(buffer), MSG_PIPE_MODE_FULL, &size, &timeout); break;
    }
    time_after = timeout;
    bits_after = bits;
    moved = size;
    sceKernelDeleteCallback(callback);
    return 0;
}

// Returns whether the object holds what the callback's second run gave
static int holds_gift(void) {
    SceUInt32 bits = 0;
    SceUInt64 user_data = 0;
    SceSize size = 0;
    char buffer[MESSAGE_SIZE];
    switch (kind) {
    case SEMA: return sceKernelPollSema(object, 1) == 0;
    case FLAG: return sceKernelPollEventFlag(object, 0x1, SCE_EVENT_WAITOR, &bits) == 0;
    case EVENT: return sceKernelPollEvent(object, EVENT_BIT_A, &bits, &user_data) == 0;
    case PIPE: return sceKernelTryReceiveMsgPipe(object, buffer, sizeof(buffer), MSG_PIPE_MODE_FULL, &size) == 0;
    }
    return 0;
}

static void run_case(const char *name, Kind k) {
    kind = k;
    callback = 0;
    callback_runs = 0;
    wait_result = NOT_DONE;
    time_after = NOT_DONE;
    bits_after = NOT_DONE;
    moved = NOT_DONE;
    switch (k) {
    case SEMA: object = sceKernelCreateSema("sema", 0, 0, 1, NULL); break;
    case FLAG: object = sceKernelCreateEventFlag("flag", 0, 0, NULL); break;
    case EVENT: object = sceKernelCreateSimpleEvent("event", 0, 0, NULL); break;
    case PIPE: object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL); break;
    }

    test_join_thread(test_start_thread(t_entry));

    PRINTF(wait_result, "%s: the wait", name);
    PRINTF(time_after, "%s: the time left", name);
    if (k == FLAG)
        PRINTF(bits_after, "%s: the out bits", name);
    if (k == PIPE)
        PRINTF(moved, "%s: the size received", name);
    PRINTF(callback_runs, "%s: runs of the callback", name);
    PRINTF(holds_gift(), "%s: the object holds what the second run gave", name);

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
