// What an event flag wait that fails leaves in its out bits. Every wait starts with the out bits
// set to 0xDEAD and prints its result and the out bits it left, once as sceKernelWaitEventFlag and
// once as sceKernelWaitEventFlagCB, with no callback pending. Every wait has a finite timeout and
// every case that needs a flag has a fresh one with the bits 0x3 set. The cases:
// - an unknown UID
// - the pattern 0
// - the mode 0x8
// - both clear modes together
// - a timeout of 0 for the bit 0x4, which is not set
// - a timeout of 100 ms for the bit 0x4, which passes
// - a flag without SCE_EVENT_WAITMULTIPLE that another thread waits on, for the bit 0x8, while the
//   main thread waits for the bit 0x1, which is set. The main thread then sets the bit 0x8, and
//   the other thread's result is printed
//
// On a PS Vita TV on firmware 3.65 a wait that the call itself refuses, with an unknown UID
// (0x800280E1), the pattern 0 (0x80020005), a bad mode (0x80020010) or another thread's wait on
// the flag (0x800280E2), leaves 0 in the out bits, and so does the same call with callbacks. A
// wait whose timeout passes writes the bits the flag has, 0x3, with the result 0x80028005. That
// holds for the timeout of 0 and for the one of 100 ms.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_WRITTEN 0xDEAD
#define UNKNOWN_UID 0x12345
#define SHORT_TIMEOUT 100000

typedef int (*WaitCall)(int, unsigned int, unsigned int, unsigned int *, SceUInt *);

static const struct {
    const char *name;
    WaitCall wait;
} kinds[] = {
    { "wait", sceKernelWaitEventFlag },
    { "wait with callbacks", sceKernelWaitEventFlagCB },
};

static SceUID flag;
static volatile int holder_result = NOT_WRITTEN;

// Waits for the bit 0x8, which is not set, until the main thread sets it
static int holder_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    unsigned int bits;
    SceUInt timeout = 2 * TEST_WAIT_US;
    holder_result = sceKernelWaitEventFlag(flag, 0x8, SCE_EVENT_WAITOR, &bits, &timeout);
    return 0;
}

// Makes the wait of kind with the out bits at 0xDEAD and prints what it returned and left
static void wait_and_print(const char *what, int kind, SceUID id, unsigned int pattern, unsigned int mode, SceUInt timeout) {
    unsigned int bits = NOT_WRITTEN;
    const int result = kinds[kind].wait(id, pattern, mode, &bits, &timeout);
    PRINTF(result, "%s, %s: the result", what, kinds[kind].name);
    PRINTF(bits, "%s, %s: the out bits", what, kinds[kind].name);
}

// Makes both kinds of wait on a fresh flag with the bits 0x3, or on the unknown UID
static void both_kinds(const char *what, int unknown, unsigned int pattern, unsigned int mode, SceUInt timeout) {
    for (int kind = 0; kind < 2; kind++) {
        const SceUID id = unknown ? UNKNOWN_UID : sceKernelCreateEventFlag("flag", 0, 0x3, NULL);
        wait_and_print(what, kind, id, pattern, mode, timeout);
        if (!unknown)
            sceKernelDeleteEventFlag(id);
    }
}

int main(void) {
    both_kinds("unknown UID", 1, 0x1, SCE_EVENT_WAITOR, SHORT_TIMEOUT);
    both_kinds("pattern 0", 0, 0x0, SCE_EVENT_WAITOR, SHORT_TIMEOUT);
    both_kinds("mode 0x8", 0, 0x1, 0x8, SHORT_TIMEOUT);
    both_kinds("both clear modes", 0, 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR | SCE_EVENT_WAITCLEAR_PAT, SHORT_TIMEOUT);
    both_kinds("timeout of 0", 0, 0x4, SCE_EVENT_WAITOR, 0);
    both_kinds("timeout of 100 ms", 0, 0x4, SCE_EVENT_WAITOR, SHORT_TIMEOUT);

    // Another thread's wait holds a flag without the multi attribute. Last, since it needs a thread
    flag = sceKernelCreateEventFlag("flag", 0, 0x3, NULL);
    const SceUID holder = test_start_thread(holder_entry);
    TEST_WAIT_FOR(test_in_wait(holder, WAIT_TYPE_EVENT_FLAG, flag));
    PRINT("held by another thread: that thread waits", test_in_wait(holder, WAIT_TYPE_EVENT_FLAG, flag));
    for (int kind = 0; kind < 2; kind++)
        wait_and_print("held by another thread", kind, flag, 0x1, SCE_EVENT_WAITOR, SHORT_TIMEOUT);
    sceKernelSetEventFlag(flag, 0x8);
    test_join_thread(holder);
    PRINT("held by another thread: that thread's result", holder_result);
    sceKernelDeleteEventFlag(flag);

    test_exit();
    return 0;
}
