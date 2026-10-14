// How a thread's stack free size is measured: next to the room below the stack pointer, after the
// thread used more stack and came back up, by another thread while it waits, after it ended, and
// in a run after a restart. Also what sceKernelCheckThreadStack returns and what the bottom of a
// new stack holds. Sizes are printed as comparisons with wide margins.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define STACK_SIZE 0x4000
// How much stack use_stack touches below its caller
#define USED 0x1000

static SceUID sema;
static volatile int runs;
static volatile int free_start[2], free_after_use[2], below_sp_start[2], below_sp_after[2], check[2], below_sp_check[2];
static volatile int bottom_uniform, bottom_value;
static SceUIntPtr stack_base;

static int below_sp(void) {
    volatile int local = 0;
    return (int)((SceUIntPtr)&local - stack_base);
}

static volatile char sink;

// Writes USED bytes of stack and returns
static __attribute__((noinline)) void use_stack(void) {
    volatile char buf[USED];
    for (int i = 0; i < USED; i++)
        buf[i] = (char)i;
    sink = buf[USED - 1];
}

static int near(int a, int b) {
    return a - b < 0x400 && b - a < 0x400;
}

static int thread_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    const int r = runs++;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    stack_base = (SceUIntPtr)info.stack;
    if (r == 0) {
        // The lowest 256 bytes, which nothing has used yet
        const SceUInt32 *bottom = (const SceUInt32 *)stack_base;
        bottom_uniform = 1;
        for (int i = 1; i < 64; i++)
            bottom_uniform &= bottom[i] == bottom[0];
        bottom_value = bottom[0];
    }
    free_start[r] = sceKernelGetThreadStackFreeSize(0);
    below_sp_start[r] = below_sp();
    use_stack();
    free_after_use[r] = sceKernelGetThreadStackFreeSize(0);
    below_sp_after[r] = below_sp();
    check[r] = sceKernelCheckThreadStack();
    below_sp_check[r] = below_sp();
    if (r == 0) {
        SceUInt timeout = TEST_WAIT_US;
        sceKernelWaitSema(sema, 1, &timeout);
    }
    return 0;
}

int main(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID thread = sceKernelCreateThread("stack", thread_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, STACK_SIZE, 0, 0, NULL);
    const int never_started = sceKernelGetThreadStackFreeSize(thread);
    PRINT("a thread not started yet: its free size is its stack size", never_started == STACK_SIZE);
    PRINT("a thread not started yet: its free size is within 1 KiB of its stack size", STACK_SIZE - never_started < 0x400);
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema));
    const int while_waiting = sceKernelGetThreadStackFreeSize(thread);
    sceKernelSignalSema(sema, 1);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    const int after_end = sceKernelGetThreadStackFreeSize(thread);
    sceKernelStartThread(thread, 0, NULL);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    const int after_second_end = sceKernelGetThreadStackFreeSize(thread);
    sceKernelDeleteThread(thread);

    PRINT("the bottom 256 bytes of a new stack hold one value", bottom_uniform);
    PRINT("that value", bottom_uniform ? bottom_value : 0);
    for (int r = 0; r < 2; r++) {
        const char *run = r == 0 ? "first run" : "second run";
        PRINTF(near(free_start[r], below_sp_start[r]), "%s: the free size at the start is the room below the stack pointer", run);
        PRINTF(free_start[r] < below_sp_start[r] - 0x400, "%s: the free size at the start is over 1 KiB less than that room", run);
        PRINTF(free_start[r] > STACK_SIZE, "%s: the free size at the start is more than the stack size", run);
        PRINTF(free_start[r] - free_after_use[r] >= USED - 0x100, "%s: after using 4 KiB more, the free size went down by that", run);
        PRINTF(free_start[r] - free_after_use[r] > 3 * USED, "%s: after using 4 KiB more, the free size went down by over 12 KiB", run);
        PRINTF(near(free_after_use[r], below_sp_after[r]), "%s: after using 4 KiB more, it is the room below the stack pointer", run);
        PRINTF(near(check[r], below_sp_check[r]), "%s: CheckThreadStack is the room below the stack pointer", run);
        PRINTF(near(check[r], free_after_use[r]), "%s: CheckThreadStack is near the free size", run);
    }
    PRINT("the second run starts with the free size the first one started with", free_start[1] == free_start[0]);
    PRINT("the second run starts with the free size the first one ended with", near(free_start[1], free_after_use[0]));
    PRINT("while it waits: another thread sees the free size it had", near(while_waiting, free_after_use[0]));
    PRINT("after it ended: the free size is its stack size", after_end == STACK_SIZE);
    PRINT("after it ended: the free size is what it had", near(after_end, free_after_use[0]));
    PRINT("after the second run: the free size is its stack size", after_second_end == STACK_SIZE);
    PRINT("after the second run: the free size is what it had", near(after_second_end, free_after_use[1]));

    test_exit();
    return 0;
}
