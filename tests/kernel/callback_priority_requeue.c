// A thread waits on a priority ordered semaphore with callbacks, and its callback waits on another
// one. Raising its priority then: whether it moves ahead in the queue of each wait. Each semaphore
// gets one signal, which goes to the first waiter in its queue, and then a cancel, which ends the
// other waits, so the results tell who was first.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID outer, inner, cb;
static volatile int outer_result, inner_result, other_result[2];

static int inner_wait_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    SceUInt timeout = TEST_WAIT_US;
    inner_result = sceKernelWaitSema(inner, 1, &timeout);
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    cb = sceKernelCreateCallback("inner", 0, inner_wait_callback, NULL);
    SceUInt timeout = TEST_WAIT_US;
    outer_result = sceKernelWaitSemaCB(outer, 1, &timeout);
    sceKernelDeleteCallback(cb);
    return 0;
}

static int other_waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = TEST_WAIT_US;
    other_result[i] = sceKernelWaitSema(i == 0 ? outer : inner, 1, &timeout);
    return 0;
}

static SceUID start(SceKernelThreadEntry entry, int priority, int arg) {
    const SceUID thread = sceKernelCreateThread("waiter", entry, priority, 0x4000, 0, 0, NULL);
    sceKernelStartThread(thread, sizeof(arg), &arg);
    return thread;
}

int main(void) {
    outer = sceKernelCreateSema("outer", SCE_KERNEL_ATTR_THREAD_PRIO, 0, 2, NULL);
    inner = sceKernelCreateSema("inner", SCE_KERNEL_ATTR_THREAD_PRIO, 0, 2, NULL);
    outer_result = inner_result = other_result[0] = other_result[1] = 0xDEAD;

    // The thread at 0x80 in both waits, and another thread at 0x70 behind it in each queue
    const SceUID thread = start(callback_thread, 0x80, 0);
    test_wait_until_waiting(thread);
    sceKernelNotifyCallback(cb, 1);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, inner));
    const SceUID first = start(other_waiter, 0x70, 0);
    test_wait_until_waiting(first);
    const SceUID second = start(other_waiter, 0x70, 1);
    test_wait_until_waiting(second);

    PRINT("raise the thread to 0x60", sceKernelChangeThreadPriority(thread, 0x60));
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    PRINT("its priority", info.currentPriority);
    PRINT("it still waits in its callback", info.waitType == WAIT_TYPE_SEMA && info.waitId == inner);

    int canceled_outer = 0xDEAD, canceled_inner = 0xDEAD;
    sceKernelSignalSema(outer, 1);
    sceKernelCancelSema(outer, 0, &canceled_outer);
    sceKernelSignalSema(inner, 1);
    sceKernelCancelSema(inner, 0, &canceled_inner);
    test_join_thread(thread);
    test_join_thread(first);
    test_join_thread(second);
    PRINT("the outer semaphore: waits its cancel ended", canceled_outer);
    PRINT("the outer semaphore: the raised thread's wait", outer_result);
    PRINT("the outer semaphore: the other thread's wait", other_result[0]);
    PRINT("the inner semaphore: waits its cancel ended", canceled_inner);
    PRINT("the inner semaphore: the raised thread's wait", inner_result);
    PRINT("the inner semaphore: the other thread's wait", other_result[1]);

    test_exit();
    return 0;
}
