// What sceKernelSuspendThreadForVM and sceKernelResumeThreadForVM do in the cases
// thread_status_matrix leaves out. The main thread runs on core 1, and the thread T it suspends
// runs on core 0 with the VM attribute. Every case reads what it prints before it prints, and
// checks progress over 50 ms:
// - T spinning, suspended twice, then resumed three times: each call's result, whether T makes
//   progress after each, and its status while one suspend is left
// - T dormant: the suspend's result and T's status, a resume's result, then whether T runs once
//   started, and after a resume
// - T dormant again after it ended: its status, and a suspend's and a resume's result
// - T suspending itself: whether the call returns before a resume, its result, and T's status
// - T running a callback that spins in a wait with callbacks: the suspend stops the callback, with
//   T's status and wait type while it holds, the callback goes on after the resume, and T's wait
//   ends normally after the callback returns
// - T waiting with callbacks, suspended, then its callback notified: whether the callback runs
//   while T is suspended, T's status and wait type, and whether it runs after the resume
// - T waiting on a semaphore that's deleted, or whose waiters are cancelled, while T is suspended:
//   T's status and wait type, whether it woke, and its wait's result after the resume
// - T delaying 100 ms, suspended for 500 ms: T's status after the delay passed, and whether it goes
//   on only after the resume
// - a delete of T while it's suspended
// - T waiting, suspended twice, then signaled: T's status, whether one resume wakes it, and its
//   wait's result after the second
//
// On a PS Vita TV on firmware 3.65 each suspend returns the count of suspends so far and each
// resume the count left, and a resume of a thread that isn't suspended fails with 0x80028033. A
// dormant thread, never started or ended, can't be suspended or resumed (0x80028027, which a resume
// checks before the count), nor can a thread suspend itself (0x80028022), nor be deleted while
// suspended (0x80028028). A suspend stops a running callback, and a thread whose wait ends while
// suspended, by a signal, a delete (0x80028006), a cancel (0x80028007), the end of a delay or a
// callback notify, shows as SUSPENDED with no wait type, and goes on only after the last resume.
// The notified callback runs only then, and its thread goes back to WAITING after it.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define T_PRIORITY 0xA0
#define SPINS 400000000

enum Mode { SPIN, WAIT, WAIT_CB, DELAY, SELF_SUSPEND };

static SceUID sema, t_uid;
static volatile int mode, ran, woke, stop, wait_result, self_result, callback_uid;
static volatile int cb_spin, cb_running, cb_stop, cb_done, cb_calls;
static volatile unsigned int counter;

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    cb_calls++;
    if (cb_spin) {
        cb_running = 1;
        for (int n = 0; n < SPINS && !cb_stop; n++)
            counter++;
        cb_done = 1;
    }
    return 0;
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    ran = 1;
    SceUInt timeout = 3 * TEST_WAIT_US;
    switch (mode) {
    case WAIT: wait_result = sceKernelWaitSema(sema, 1, &timeout); break;
    case WAIT_CB:
        callback_uid = sceKernelCreateCallback("cb", 0, callback_entry, NULL);
        wait_result = sceKernelWaitSemaCB(sema, 1, &timeout);
        break;
    case DELAY: sceKernelDelayThread(100000); break;
    case SELF_SUSPEND: self_result = sceKernelSuspendThreadForVM(t_uid); break;
    }
    woke = 1;
    for (int n = 0; n < SPINS && !stop; n++)
        counter++;
    return 0;
}

// Starts T in a mode, with every flag reset
static SceUID start_t(int new_mode) {
    mode = new_mode;
    ran = woke = stop = 0;
    wait_result = self_result = callback_uid = 0xDEAD;
    cb_spin = cb_running = cb_stop = cb_done = cb_calls = 0;
    counter = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    t_uid = sceKernelCreateThread("t", t_entry, T_PRIORITY, 0x4000, THREAD_ATTR_VM, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t_uid, 0, NULL);
    return t_uid;
}

static void end_t(SceUID t) {
    stop = 1;
    cb_stop = 1;
    test_join_thread(t);
    sceKernelDeleteSema(sema);
}

// Whether counter moves within 50 ms
static int progresses(void) {
    const unsigned int before = counter;
    sceKernelDelayThread(50000);
    return counter != before;
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

static SceUInt32 wait_type_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.waitType;
}

static void counts(void) {
    const SceUID t = start_t(SPIN);
    TEST_WAIT_FOR(ran);
    const int first = sceKernelSuspendThreadForVM(t);
    const int second = sceKernelSuspendThreadForVM(t);
    const int held = progresses();
    const int resume1 = sceKernelResumeThreadForVM(t);
    const int held_after_one = progresses();
    const SceUInt32 status = status_of(t);
    const int resume2 = sceKernelResumeThreadForVM(t);
    const int runs = progresses();
    const int resume3 = sceKernelResumeThreadForVM(t);
    end_t(t);
    PRINT("suspended twice: the first suspend", first);
    PRINT("suspended twice: the second suspend", second);
    PRINT("suspended twice: it makes progress", held);
    PRINT("suspended twice: the first resume", resume1);
    PRINT("suspended twice: it makes progress after one resume", held_after_one);
    PRINT("suspended twice: its status after one resume", status);
    PRINT("suspended twice: the second resume", resume2);
    PRINT("suspended twice: it makes progress after both", runs);
    PRINT("suspended twice: a third resume", resume3);
}

static void dormant(void) {
    mode = SPIN;
    ran = stop = 0;
    counter = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID t = sceKernelCreateThread("t", t_entry, T_PRIORITY, 0x4000, THREAD_ATTR_VM, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    const int suspended = sceKernelSuspendThreadForVM(t);
    const SceUInt32 status = status_of(t);
    const int resumed_dormant = sceKernelResumeThreadForVM(t);
    const int started = sceKernelStartThread(t, 0, NULL);
    sceKernelDelayThread(50000);
    const int ran_started = ran;
    const int resumed = sceKernelResumeThreadForVM(t);
    TEST_WAIT_FOR(ran);
    const int ran_resumed = ran;
    end_t(t);
    PRINT("dormant: suspend it", suspended);
    PRINT("dormant: its status after", status);
    PRINT("dormant: resume it before the start", resumed_dormant);
    PRINT("dormant: start it", started);
    PRINT("dormant: it ran within 50 ms of the start", ran_started);
    PRINT("dormant: resume it", resumed);
    PRINT("dormant: it ran after the resume", ran_resumed);
}

static void ended(void) {
    const SceUID t = start_t(SPIN);
    TEST_WAIT_FOR(ran);
    stop = 1;
    sceKernelWaitThreadEnd(t, NULL, NULL);
    const SceUInt32 status = status_of(t);
    const int suspended = sceKernelSuspendThreadForVM(t);
    const int resumed = sceKernelResumeThreadForVM(t);
    sceKernelDeleteThread(t);
    sceKernelDeleteSema(sema);
    PRINT("ended: its status", status);
    PRINT("ended: suspend it", suspended);
    PRINT("ended: resume it", resumed);
}

static void self_suspend(void) {
    const SceUID t = start_t(SELF_SUSPEND);
    TEST_WAIT_FOR(woke);
    const int returned = woke;
    const SceUInt32 status = status_of(t);
    const int resumed = sceKernelResumeThreadForVM(t);
    TEST_WAIT_FOR(woke);
    end_t(t);
    PRINT("suspends itself: the call returned before a resume", returned);
    PRINT("suspends itself: its result", self_result);
    PRINT("suspends itself: its status then", status);
    PRINT("suspends itself: a resume", resumed);
}

static void in_callback(void) {
    const SceUID t = start_t(WAIT_CB);
    cb_spin = 1;
    test_wait_until_waiting(t);
    sceKernelNotifyCallback(callback_uid, 1);
    TEST_WAIT_FOR(cb_running);
    const int suspended = sceKernelSuspendThreadForVM(t);
    const int held = progresses();
    const SceUInt32 status = status_of(t), wait_type = wait_type_of(t);
    const int resumed = sceKernelResumeThreadForVM(t);
    const int runs = progresses();
    cb_stop = 1;
    TEST_WAIT_FOR(cb_done);
    test_wait_until_waiting(t);
    const SceUInt32 status_back = status_of(t);
    sceKernelSignalSema(sema, 1);
    TEST_WAIT_FOR(woke);
    end_t(t);
    PRINT("in a callback: suspend it", suspended);
    PRINT("in a callback: the callback makes progress", held);
    PRINT("in a callback: its status", status);
    PRINT("in a callback: its wait type", wait_type);
    PRINT("in a callback: resume it", resumed);
    PRINT("in a callback: the callback makes progress after the resume", runs);
    PRINT("in a callback: its status once the callback returned", status_back);
    PRINT("in a callback: its wait's result", wait_result);
}

static void notified_while_suspended(void) {
    const SceUID t = start_t(WAIT_CB);
    test_wait_until_waiting(t);
    const int suspended = sceKernelSuspendThreadForVM(t);
    sceKernelNotifyCallback(callback_uid, 1);
    sceKernelDelayThread(50000);
    const int calls_held = cb_calls;
    const SceUInt32 status = status_of(t), wait_type = wait_type_of(t);
    const int resumed = sceKernelResumeThreadForVM(t);
    TEST_WAIT_FOR(cb_calls > 0);
    const int calls_after = cb_calls;
    test_wait_until_waiting(t);
    const SceUInt32 status_back = status_of(t);
    sceKernelSignalSema(sema, 1);
    TEST_WAIT_FOR(woke);
    end_t(t);
    PRINT("notified while suspended: suspend it", suspended);
    PRINT("notified while suspended: the callback ran while suspended", calls_held);
    PRINT("notified while suspended: its status", status);
    PRINT("notified while suspended: its wait type", wait_type);
    PRINT("notified while suspended: resume it", resumed);
    PRINT("notified while suspended: the callback ran after the resume", calls_after);
    PRINT("notified while suspended: its status after the callback", status_back);
    PRINT("notified while suspended: its wait's result", wait_result);
}

// T's wait ends by a delete of its semaphore, or a cancel of its waiters, while T is suspended
static void ended_while_suspended(const char *name, int by_delete) {
    const SceUID t = start_t(WAIT);
    test_wait_until_waiting(t);
    const int suspended = sceKernelSuspendThreadForVM(t);
    const int ended = by_delete ? sceKernelDeleteSema(sema) : sceKernelCancelSema(sema, -1, NULL);
    sceKernelDelayThread(50000);
    const int woke_held = woke;
    const SceUInt32 status = status_of(t), wait_type = wait_type_of(t);
    const int resumed = sceKernelResumeThreadForVM(t);
    TEST_WAIT_FOR(woke);
    stop = 1;
    test_join_thread(t);
    if (!by_delete)
        sceKernelDeleteSema(sema);
    PRINTF(suspended, "%s while suspended: suspend it", name);
    PRINTF(ended, "%s while suspended: the %s", name, name);
    PRINTF(woke_held, "%s while suspended: it woke before the resume", name);
    PRINTF(status, "%s while suspended: its status", name);
    PRINTF(wait_type, "%s while suspended: its wait type", name);
    PRINTF(resumed, "%s while suspended: resume it", name);
    PRINTF(wait_result, "%s while suspended: its wait's result", name);
}

static void delay_passes(void) {
    const SceUID t = start_t(DELAY);
    test_wait_until_waiting(t);
    const int suspended = sceKernelSuspendThreadForVM(t);
    sceKernelDelayThread(500000);
    const int woke_held = woke;
    const SceUInt32 status = status_of(t), wait_type = wait_type_of(t);
    const int resumed = sceKernelResumeThreadForVM(t);
    TEST_WAIT_FOR(woke);
    const int woke_resumed = woke;
    end_t(t);
    PRINT("a delay that passes while suspended: suspend it", suspended);
    PRINT("a delay that passes while suspended: it went on before the resume", woke_held);
    PRINT("a delay that passes while suspended: its status", status);
    PRINT("a delay that passes while suspended: its wait type", wait_type);
    PRINT("a delay that passes while suspended: resume it", resumed);
    PRINT("a delay that passes while suspended: it went on after the resume", woke_resumed);
}

static void delete_suspended(void) {
    const SceUID t = start_t(SPIN);
    TEST_WAIT_FOR(ran);
    const int suspended = sceKernelSuspendThreadForVM(t);
    const int deleted = sceKernelDeleteThread(t);
    const int resumed = sceKernelResumeThreadForVM(t);
    end_t(t);
    PRINT("delete it while suspended: suspend it", suspended);
    PRINT("delete it while suspended: the delete", deleted);
    PRINT("delete it while suspended: resume it", resumed);
}

static void twice_then_signaled(void) {
    const SceUID t = start_t(WAIT);
    test_wait_until_waiting(t);
    const int first = sceKernelSuspendThreadForVM(t);
    const int second = sceKernelSuspendThreadForVM(t);
    sceKernelSignalSema(sema, 1);
    // The woken thread takes a moment to stop
    sceKernelDelayThread(50000);
    const SceUInt32 status = status_of(t);
    const int resume1 = sceKernelResumeThreadForVM(t);
    sceKernelDelayThread(50000);
    const int woke_one = woke;
    const SceUInt32 status_one = status_of(t);
    const int resume2 = sceKernelResumeThreadForVM(t);
    TEST_WAIT_FOR(woke);
    end_t(t);
    PRINT("waiting, suspended twice: the first suspend", first);
    PRINT("waiting, suspended twice: the second suspend", second);
    PRINT("waiting, suspended twice, signaled: its status 50 ms later", status);
    PRINT("waiting, suspended twice, signaled: the first resume", resume1);
    PRINT("waiting, suspended twice, signaled: it woke after one resume", woke_one);
    PRINT("waiting, suspended twice, signaled: its status after one resume", status_one);
    PRINT("waiting, suspended twice, signaled: the second resume", resume2);
    PRINT("waiting, suspended twice, signaled: its wait's result", wait_result);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    counts();
    dormant();
    ended();
    self_suspend();
    in_callback();
    notified_while_suspended();
    ended_while_suspended("delete", 1);
    ended_while_suspended("cancel", 0);
    delay_passes();
    delete_suspended();
    twice_then_signaled();
    test_exit();
    return 0;
}
