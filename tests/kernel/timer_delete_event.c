// What the waiters of a timer get when the timer is deleted. Thread A waits on the timer for the
// delete bit 0x8, and thread B for the timer bit 0x8000, both with a timeout of 8 s. Once both
// wait, the main thread deletes the timer. Each wait prints its result, the bits it got and
// whether its user data's high word is the deleting thread's UID and its low word the process ID.
// The out values start as 0xDEAD.
//
// On a PS Vita TV on firmware 3.65 the wait for the delete bit returns 0 with 0x8 and the deleting
// thread's user data, and the wait for the timer bit returns 0x80028006 with the error bit 0x10 and
// no user data.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD

static SceUID timer;
static volatile int results[2];
static volatile SceUInt32 bits[2];
static volatile SceUInt64 user_data[2];

static int waiter_entry(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 timeout = 4 * TEST_WAIT_US;
    SceUInt32 got = NOT_DONE;
    SceUInt64 data = NOT_DONE;
    results[i] = sceKernelWaitEvent(timer, i == 0 ? EVENT_DELETE : EVENT_TIMER, &got, &data, &timeout);
    bits[i] = got;
    user_data[i] = data;
    return 0;
}

int main(void) {
    const SceUID self = sceKernelGetThreadId();
    const SceUID process = sceKernelGetProcessId();
    timer = sceKernelCreateTimer("delete", 0, NULL);
    SceUID threads[2];
    for (int i = 0; i < 2; i++) {
        results[i] = NOT_DONE;
        threads[i] = test_start_thread_arg(waiter_entry, i);
        TEST_WAIT_FOR(test_in_wait(threads[i], WAIT_TYPE_EVENT, timer));
    }
    PRINT("the delete", sceKernelDeleteTimer(timer));
    for (int i = 0; i < 2; i++)
        test_join_thread(threads[i]);

    static const char *const names[] = { "the wait for the delete bit", "the wait for the timer bit" };
    for (int i = 0; i < 2; i++) {
        PRINTF(results[i], "%s", names[i]);
        PRINTF(bits[i], "%s: the bits it got", names[i]);
        PRINTF((SceUInt32)(user_data[i] >> 32) == (SceUInt32)self, "%s: the high word of its user data is the deleting thread's UID", names[i]);
        PRINTF((SceUInt32)user_data[i] == (SceUInt32)process, "%s: the low word of its user data is the process ID", names[i]);
    }
    test_exit();
    return 0;
}
