// The out bits of an event flag wait that the flag's delete ends. The flag starts with 0x4, and
// thread T waits with callbacks for 0x1 with a timeout of 8 s. While T waits, the main thread sets
// 0x8, which doesn't end the wait. Then either:
// - queued: the main thread deletes the flag while T waits
// - in its callback: the main thread notifies T's callback, which blocks on a gate semaphore, sets
//   0x2 while T is out of the flag's queue, deletes the flag and signals the gate
// Each case prints T's wait result and its out bits, which start as 0xDEAD. The second case first
// prints whether T's callback waits on the gate when the flag is deleted.
//
// On a PS Vita TV on firmware 3.65 both waits return 0x80028006 with the flag's bits at the delete:
// 0xC when the delete ends the queued wait, and 0xE when it comes while T's callback runs, with the
// bit set during the callback.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD

static SceUID flag, gate;
static volatile SceUID callback;
static volatile int t_result;
static volatile SceUInt32 t_bits;

// Blocks on the gate, so T stays out of the flag's queue until the main thread lets it go
static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    SceUInt32 timeout = 4 * TEST_WAIT_US;
    sceKernelWaitSema(gate, 1, &timeout);
    return 0;
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("delete bits", 0, callback_entry, NULL);
    SceUInt32 timeout = 4 * TEST_WAIT_US;
    SceUInt32 bits = NOT_DONE;
    t_result = sceKernelWaitEventFlagCB(flag, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
    t_bits = bits;
    sceKernelDeleteCallback(callback);
    return 0;
}

static void run_case(const char *name, int in_callback) {
    t_result = NOT_DONE;
    t_bits = NOT_DONE;
    flag = sceKernelCreateEventFlag("flag", 0, 0x4, NULL);
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);
    const SceUID t = test_start_thread(t_entry);
    TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB, flag));
    sceKernelSetEventFlag(flag, 0x8);
    int in_gate = 0;
    if (in_callback) {
        sceKernelNotifyCallback(callback, 1);
        TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, gate));
        in_gate = test_in_wait(t, WAIT_TYPE_SEMA, gate);
        sceKernelSetEventFlag(flag, 0x2);
    }
    sceKernelDeleteEventFlag(flag);
    sceKernelSignalSema(gate, 1);
    test_join_thread(t);
    sceKernelDeleteSema(gate);
    if (in_callback)
        PRINTF(in_gate, "%s: T's callback waits on the gate when the flag is deleted", name);
    PRINTF(t_result, "%s: T's wait", name);
    PRINTF(t_bits, "%s: T's out bits", name);
}

int main(void) {
    run_case("queued", 0);
    run_case("in its callback", 1);
    test_exit();
    return 0;
}
