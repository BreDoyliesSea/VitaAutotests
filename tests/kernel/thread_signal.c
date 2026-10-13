// Thread signals, sceKernelSendSignal and sceKernelWaitSignal, where a signal is always there to
// take: a signal sent before the wait, a second one before the first is taken, the arguments of
// the wait, a wait with callbacks, a thread woken by a signal, and signals to UIDs that aren't
// running threads. The waits that wait for a signal are in thread_signal_timeout.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/signal.h>

#include "test.h"
#include "threadmgr_extra.h"

SceInt32 sceKernelWaitSignalCB(SceUInt32 unk0, SceUInt32 delay, SceUInt32 *timeout);

static volatile int wait_result, calls;

static int signal_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    wait_result = sceKernelWaitSignal(0, 0, &timeout);
    return 0;
}

static int counting_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    calls++;
    return 0;
}

static int dormant_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

int main(void) {
    const SceUID self = sceKernelGetThreadId();
    SceUInt32 timeout;

    PRINT("send a signal to this thread", sceKernelSendSignal(self));
    PRINT("send a second one before it is taken", sceKernelSendSignal(self));
    timeout = 1000;
    PRINT("wait with a signal sent", sceKernelWaitSignal(0, 0, &timeout));
    PRINT("its time left is all of it", timeout == 1000);
    sceKernelSendSignal(self);
    PRINT("wait without a timeout with a signal sent", sceKernelWaitSignal(0, 0, NULL));
    sceKernelSendSignal(self);
    timeout = 1000;
    PRINT("wait with 1 as the first argument", sceKernelWaitSignal(1, 0, &timeout));

    // The delay, with a signal already sent: either it waits 200 ms first or it ends at once
    sceKernelSendSignal(self);
    timeout = 1000000;
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    const int delayed = sceKernelWaitSignal(0, 200000, &timeout);
    const SceUInt64 elapsed = sceKernelGetProcessTimeWide() - start;
    PRINT("wait with a delay of 200 ms and a signal sent", delayed);
    PRINT("it took longer than 100 ms", elapsed >= 100000);

    // With callbacks
    const SceUID cb = sceKernelCreateCallback("signal", 0, counting_callback, NULL);
    sceKernelSendSignal(self);
    sceKernelNotifyCallback(cb, 1);
    timeout = 1000;
    PRINT("wait with callbacks, a signal sent and a callback pending", sceKernelWaitSignalCB(0, 0, &timeout));
    PRINT("runs of the callback", calls);
    sceKernelDeleteCallback(cb);

    // A thread woken by a signal
    wait_result = 0xDEAD;
    const SceUID waiter = test_start_thread(signal_waiter);
    test_wait_until_waiting(waiter);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(waiter, &info);
    PRINT("a thread waiting for a signal: its wait type", info.waitType);
    PRINT("its wait UID is itself", info.waitId == waiter);
    PRINT("its wait UID is 0", info.waitId == 0);
    PRINT("send it a signal", sceKernelSendSignal(waiter));
    test_join_thread(waiter);
    PRINT("its wait", wait_result);

    // UIDs that aren't running threads, last, so an implementation that crashes on them loses nothing else
    const SceUID dormant = sceKernelCreateThread("dormant", dormant_entry, 0x10000100, 0x4000, 0, 0, NULL);
    PRINT("send a signal to a thread that never started", sceKernelSendSignal(dormant));
    sceKernelDeleteThread(dormant);
    const SceUID sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    PRINT("send a signal to a semaphore", sceKernelSendSignal(sema));
    PRINT("send a signal to an unknown UID", sceKernelSendSignal(0x12345));

    test_exit();
    return 0;
}
