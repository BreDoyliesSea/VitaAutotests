// For Vita3K savestates, not for hardware: sets up one of every kind of kernel object with threads
// blocked on them in the kinds of call a savestate has to make again, then sleeps through the
// moment Vita3K, run with VITA3K_SNAPSHOT_ROUNDTRIP=1, saves a state and loads it back. Then it
// prints what the guest can see of each object and wakes every thread in a fixed order. The output
// has to be the same with and without the round trip, so its .expected comes from a run without.
//
// The threads, blocked during the sleep:
// - a semaphore wait for 3 with 2 there
// - an event flag wait for 0x3 with clear, with 0x5 set
// - a worker holding a recursive mutex twice, waiting on the event flag for 0x100 to unlock it
// - a condition variable wait for the signal, and one signalled but waiting to take the mutex back,
//   which the main thread holds
// - a 3 s delay, and a wait for the end of the delayed thread
// - a message pipe with 100 bytes in it, and a callback of the main thread with a notification
// The main thread holds two read locks of a rwlock and sleeps 2 s.
#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define MUTEX_RECURSIVE 0x2
#define EVF_WAITMULTIPLE 0x1000

static SceUID sema, flag, mutex, cond_mutex, cond, rwlock, pipe, delayed;
static volatile int sema_result = 1, flag_result = 1, worker_unlock = 1, cond_result = 1, relock_result = 1, delay_result = 1, end_result = 1;
static volatile unsigned int flag_bits;
static volatile int callback_count, callback_arg;

static int sema_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sema_result = sceKernelWaitSema(sema, 3, NULL);
    return 0;
}

static int flag_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    unsigned int bits = 0;
    flag_result = sceKernelWaitEventFlag(flag, 0x3, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR, &bits, NULL);
    flag_bits = bits;
    return 0;
}

static int mutex_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelLockMutex(mutex, 2, NULL);
    sceKernelWaitEventFlag(flag, 0x100, SCE_EVENT_WAITAND, NULL, NULL);
    worker_unlock = sceKernelUnlockMutex(mutex, 2);
    return 0;
}

static int cond_thread(SceSize args, void *argp) {
    (void)args;
    const int relock = *(const int *)argp;
    sceKernelLockMutex(cond_mutex, 1, NULL);
    const int result = sceKernelWaitCond(cond, NULL);
    if (relock)
        relock_result = result;
    else
        cond_result = result;
    sceKernelUnlockMutex(cond_mutex, 1);
    return 0;
}

static int delay_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    delay_result = sceKernelDelayThread(3000000);
    return 0;
}

static int end_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    end_result = sceKernelWaitThreadEnd(delayed, NULL, NULL);
    return 0;
}

static int callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)common;
    callback_count = count;
    callback_arg = arg;
    return 0;
}

int main(void) {
    sema = sceKernelCreateSema("sema", 0, 2, 3, NULL);
    flag = sceKernelCreateEventFlag("flag", EVF_WAITMULTIPLE, 0x5, NULL);
    mutex = sceKernelCreateMutex("mutex", MUTEX_RECURSIVE, 0, NULL);
    cond_mutex = sceKernelCreateMutex("cond mutex", 0, 0, NULL);
    cond = sceKernelCreateCond("cond", 0, cond_mutex, NULL);
    rwlock = sceKernelCreateRWLock("rwlock", 0, NULL);
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 0x1000, NULL);
    const SceUID cb = sceKernelCreateCallback("callback", 0, callback, NULL);

    const SceUID sema_waiter = test_start_thread(sema_thread);
    test_wait_until_waiting(sema_waiter);
    const SceUID flag_waiter = test_start_thread(flag_thread);
    test_wait_until_waiting(flag_waiter);
    const SceUID worker = test_start_thread(mutex_thread);
    test_wait_until_waiting(worker);
    const SceUID cond_waiter = test_start_thread_arg(cond_thread, 0);
    test_wait_until_waiting(cond_waiter);
    // Signalled while the main thread holds the mutex, so it waits to take it back
    const SceUID relock_waiter = test_start_thread_arg(cond_thread, 1);
    test_wait_until_waiting(relock_waiter);
    sceKernelLockMutex(cond_mutex, 1, NULL);
    sceKernelSignalCondTo(cond, relock_waiter);
    delayed = test_start_thread(delay_thread);
    test_wait_until_waiting(delayed);
    const SceUID end_waiter = test_start_thread(end_thread);
    test_wait_until_waiting(end_waiter);

    char message[100];
    for (int i = 0; i < (int)sizeof(message); i++)
        message[i] = (char)i;
    PRINT("send 100 bytes", sceKernelSendMsgPipe(pipe, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL, NULL));
    sceKernelLockReadRWLock(rwlock, NULL);
    sceKernelLockReadRWLock(rwlock, NULL);
    sceKernelNotifyCallback(cb, 7);

    // The round trip happens in here
    PRINT("sleep", sceKernelDelayThread(2000000));

    SceKernelSemaInfo sema_info = { .size = sizeof(sema_info) };
    PRINT("sema info", sceKernelGetSemaInfo(sema, &sema_info));
    PRINT("sema count", sema_info.currentCount);
    PRINT("sema waiters", sema_info.numWaitThreads);
    SceKernelEventFlagInfo flag_info = { .size = sizeof(flag_info) };
    PRINT("flag info", sceKernelGetEventFlagInfo(flag, &flag_info));
    PRINT("flag pattern", flag_info.currentPattern);
    PRINT("flag waiters", flag_info.numWaitThreads);
    SceKernelMutexInfo mutex_info = { .size = sizeof(mutex_info) };
    PRINT("mutex info", sceKernelGetMutexInfo(mutex, &mutex_info));
    PRINT("mutex count", mutex_info.currentCount);
    PRINT("mutex owned by the worker", mutex_info.currentOwnerId == worker);
    SceKernelRWLockInfo rwlock_info = { .size = sizeof(rwlock_info) };
    PRINT("rwlock info", sceKernelGetRWLockInfo(rwlock, &rwlock_info));
    PRINT("rwlock count", rwlock_info.lockCount);
    SceKernelMppInfo pipe_info = { .size = sizeof(pipe_info) };
    PRINT("pipe info", sceKernelGetMsgPipeInfo(pipe, &pipe_info));
    PRINT("pipe free", pipe_info.freeSize);

    PRINT("signal sema", sceKernelSignalSema(sema, 1));
    test_join_thread(sema_waiter);
    PRINT("sema wait", sema_result);
    PRINT("set 0x2", sceKernelSetEventFlag(flag, 0x2));
    test_join_thread(flag_waiter);
    PRINT("flag wait", flag_result);
    PRINT("flag bits", flag_bits);
    PRINT("set 0x100", sceKernelSetEventFlag(flag, 0x100));
    test_join_thread(worker);
    PRINT("worker unlocks twice", worker_unlock);
    PRINT("signal cond", sceKernelSignalCond(cond));
    PRINT("unlock cond mutex", sceKernelUnlockMutex(cond_mutex, 1));
    test_join_thread(cond_waiter);
    test_join_thread(relock_waiter);
    PRINT("cond wait", cond_result);
    PRINT("signalled cond wait", relock_result);
    test_join_thread(end_waiter);
    test_join_thread(delayed);
    PRINT("delay", delay_result);
    PRINT("wait thread end", end_result);

    char received[100] = { 0 };
    PRINT("receive 100 bytes", sceKernelReceiveMsgPipe(pipe, received, sizeof(received), MSG_PIPE_MODE_FULL, NULL, NULL));
    PRINT("same bytes", sceClibMemcmp(received, message, sizeof(message)) == 0);
    PRINT("unlock read", sceKernelUnlockReadRWLock(rwlock));
    PRINT("unlock read again", sceKernelUnlockReadRWLock(rwlock));
    PRINT("check callback", sceKernelCheckCallback());
    PRINT("callback count", callback_count);
    PRINT("callback arg", callback_arg);

    test_exit();
    return 0;
}
