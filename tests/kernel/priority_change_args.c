// What sceKernelChangeThreadPriority and sceKernelChangeThreadPriority2 take. W, at 0x90, waits on
// a semaphore while the main thread, at 0xA0, gives it each priority with both calls, and prints
// what the call returns and W's current priority from its info after it, then sets W back to
// 0x90. The priorities: the ends of the range a game may use and the values next to them, the
// ends of the system ranges, negative values, and values relative to the default. Then the second
// call with thread ID 0 and UIDs that aren't threads, and its results for a thread that never ran,
// one that ended, and the priority each run of that thread starts at.
//
// On a PS Vita TV on firmware 3.65 both calls take 0x40 to 0xBF, and the values 0x100000E0 to
// 0x1000011F relative to the default 0x10000100, which stand for 0x80 to 0xBF, and 0 stands for the
// caller's priority. Every other value, the system ranges and negative values included, gives
// 0x80028023 (ILLEGAL_PRIORITY) and leaves W's priority as it was. The second call returns the
// priority the thread had before the change, so with thread ID 0 it returns the caller's own. A UID
// that isn't a live thread gives 0x80028021 (UNKNOWN_THREAD_ID), even with 0xC0. A thread that
// never ran and one that ended take the change, and still start each run at their initial priority.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define W_PRIORITY 0x90

int sceKernelChangeThreadPriority2(SceUID thid, int priority);

static SceUID sema;
static volatile int run_priority;

static int w_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = 5 * TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    return 0;
}

static int run_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    run_priority = sceKernelGetThreadCurrentPriority();
    return 0;
}

static int current_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.currentPriority;
}

static int init_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.initPriority;
}

static void values(SceUID w) {
    static const int priorities[] = {
        0,          0x3F,       0x40,       0xBF,       0xC0,       0xFB,       0xFC,
        0xFF,       0x100,      -1,         -0x40,      (int)0x80000000,        0x7FFFFFFF,
        0x10000100, 0x100000E0, 0x100000DF, 0x1000011F, 0x10000120, 0x10000000, 0x0FFFFFFF,
    };
    for (unsigned i = 0; i < sizeof(priorities) / sizeof(priorities[0]); i++) {
        const int p = priorities[i];
        PRINTF(sceKernelChangeThreadPriority(w, p), "change W to 0x%X", p);
        PRINTF(current_of(w), "W's priority after the change to 0x%X", p);
        sceKernelChangeThreadPriority(w, W_PRIORITY);
        PRINTF(sceKernelChangeThreadPriority2(w, p), "change W to 0x%X with the second call", p);
        PRINTF(current_of(w), "W's priority after the second call with 0x%X", p);
        sceKernelChangeThreadPriority(w, W_PRIORITY);
    }
}

static int empty_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

static void ids(void) {
    PRINT("the second call with thread ID 0 and 0xA0", sceKernelChangeThreadPriority2(0, 0xA0));
    PRINT("the second call with thread ID 0 and 0", sceKernelChangeThreadPriority2(0, 0));
    PRINT("the main thread's priority after them", sceKernelGetThreadCurrentPriority());
    const SceUID deleted = sceKernelCreateThread("deleted", empty_entry, 0xA0, 0x4000, 0, 0, NULL);
    sceKernelDeleteThread(deleted);
    PRINT("the second call on an unknown UID", sceKernelChangeThreadPriority2(0x12345, 0xA0));
    PRINT("the second call on a deleted thread", sceKernelChangeThreadPriority2(deleted, 0xA0));
    PRINT("the second call on a semaphore", sceKernelChangeThreadPriority2(sema, 0xA0));
    PRINT("the second call on an unknown UID with 0xC0",
          sceKernelChangeThreadPriority2(0x12345, 0xC0));
}

static void dormant(void) {
    const SceUID d = sceKernelCreateThread("d", run_entry, 0x60, 0x4000, 0, 0, NULL);
    PRINT("the second call on a thread that never ran, to 0x70",
          sceKernelChangeThreadPriority2(d, 0x70));
    PRINT("then to 0x78", sceKernelChangeThreadPriority2(d, 0x78));
    PRINT("then to 0xC0", sceKernelChangeThreadPriority2(d, 0xC0));
    PRINT("its initial priority", init_of(d));
    PRINT("its current priority", current_of(d));
    run_priority = -1;
    sceKernelStartThread(d, 0, NULL);
    sceKernelWaitThreadEnd(d, NULL, NULL);
    PRINT("the priority its first run starts at", run_priority);
    PRINT("its current priority after it ended", current_of(d));
    PRINT("the second call on it ended, to 0x68", sceKernelChangeThreadPriority2(d, 0x68));
    PRINT("its current priority after it", current_of(d));
    PRINT("the second call on it ended, to 0", sceKernelChangeThreadPriority2(d, 0));
    PRINT("its current priority after it", current_of(d));
    sceKernelChangeThreadPriority2(d, 0x68);
    run_priority = -1;
    sceKernelStartThread(d, 0, NULL);
    sceKernelWaitThreadEnd(d, NULL, NULL);
    PRINT("the priority its second run starts at", run_priority);
    sceKernelDeleteThread(d);
}

int main(void) {
    PRINT("the main thread's priority", sceKernelGetThreadCurrentPriority());
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID w = sceKernelCreateThread("w", w_entry, W_PRIORITY, 0x4000, 0, 0, NULL);
    sceKernelStartThread(w, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(w, WAIT_TYPE_SEMA, sema));
    values(w);
    ids();
    sceKernelSignalSema(sema, 1);
    test_join_thread(w);
    dormant();
    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}
