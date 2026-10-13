// The thread ID registers: TPIDRURW, which user code may write, and TPIDRURO, which points at the
// thread's TLS. What a new thread starts with, whether both survive a delay and another thread
// running on its core, what a callback sees and whether a change made in it stays, and what a
// restart keeps. Addresses are printed as comparisons.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static unsigned int get_tpidrurw(void) {
    unsigned int value;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(value));
    return value;
}

static void set_tpidrurw(unsigned int value) {
    __asm__ volatile("mcr p15, 0, %0, c13, c0, 2" : : "r"(value));
}

static unsigned int get_tpidruro(void) {
    unsigned int value;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
    return value;
}

static SceUID sema, cb;
static volatile int phase, runs;
static volatile unsigned int new_rw, thread_ro, first_slot_matches, last_slot_matches, after_delay, after_switch;
static volatile unsigned int cb_rw, cb_ro, after_cb, check_cb_rw, after_check, restart_rw;
static volatile unsigned int other_ro, ro_after_delay, ro_after_switch, ro_after_cb, check_cb_ro, ro_after_check, restart_ro;

// Sees the thread's registers and changes TPIDRURW
static int id_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    if (phase == 0) {
        cb_rw = get_tpidrurw();
        cb_ro = get_tpidruro();
    } else {
        check_cb_rw = get_tpidrurw();
        check_cb_ro = get_tpidruro();
    }
    set_tpidrurw(0x3333);
    return 0;
}

static int id_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    runs++;
    if (runs == 2) {
        restart_rw = get_tpidrurw();
        restart_ro = get_tpidruro();
        return 0;
    }
    new_rw = get_tpidrurw();
    thread_ro = get_tpidruro();
    first_slot_matches = (SceUIntPtr)sceKernelGetTLSAddr(0) == thread_ro - 0x800;
    last_slot_matches = (SceUIntPtr)sceKernelGetTLSAddr(255) == thread_ro - 0x800 + 4 * 255;

    set_tpidrurw(0x1111);
    sceKernelDelayThread(1000);
    after_delay = get_tpidrurw();
    ro_after_delay = get_tpidruro();

    // Another thread runs on this core and sets its own TPIDRURW while this one waits
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    after_switch = get_tpidrurw();
    ro_after_switch = get_tpidruro();

    // A callback run by a wait
    set_tpidrurw(0x1111);
    cb = sceKernelCreateCallback("ids", 0, id_callback, NULL);
    sceKernelNotifyCallback(cb, 1);
    timeout = 1000;
    sceKernelWaitSemaCB(sema, 1, &timeout);
    after_cb = get_tpidrurw();
    ro_after_cb = get_tpidruro();

    // A callback run by a check
    phase = 1;
    set_tpidrurw(0x1111);
    sceKernelNotifyCallback(cb, 1);
    sceKernelCheckCallback();
    after_check = get_tpidrurw();
    ro_after_check = get_tpidruro();
    sceKernelDeleteCallback(cb);

    // What the next run starts with
    set_tpidrurw(0x4444);
    return 0;
}

static int other_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    set_tpidrurw(0x2222);
    other_ro = get_tpidruro();
    sceKernelSignalSema(sema, 1);
    return 0;
}

int main(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const unsigned int main_ro = get_tpidruro();
    PRINT("the main thread's TPIDRURO is 0x800 past its first TLS slot", (SceUIntPtr)sceKernelGetTLSAddr(0) == main_ro - 0x800);
    PRINT("the main thread's TPIDRURW", get_tpidrurw());

    // Both threads on core 1, so the other one runs on the core the first one waits on
    const SceUID thread = sceKernelCreateThread("ids", id_thread, 0x10000100, 0x4000, 0, CPU_MASK_USER_1, NULL);
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema));
    const SceUID other = sceKernelCreateThread("other", other_thread, 0x10000100, 0x4000, 0, CPU_MASK_USER_1, NULL);
    sceKernelStartThread(other, 0, NULL);
    test_join_thread(other);
    sceKernelWaitThreadEnd(thread, NULL, NULL);

    PRINT("a new thread's TPIDRURW", new_rw);
    PRINT("its TPIDRURO differs from the main thread's", thread_ro != main_ro);
    PRINT("its TPIDRURO is 0x800 past its first TLS slot", first_slot_matches);
    PRINT("and its last TLS slot is 255 words past the first", last_slot_matches);
    PRINT("after a delay: TPIDRURW", after_delay);
    PRINT("after a delay: its TPIDRURO is the same", ro_after_delay == thread_ro);
    PRINT("the other thread on its core has another TPIDRURO", other_ro != thread_ro);
    PRINT("after another thread ran on its core: TPIDRURW", after_switch);
    PRINT("after another thread ran on its core: its TPIDRURO is the same", ro_after_switch == thread_ro);
    PRINT("a callback run by a wait sees TPIDRURW", cb_rw);
    PRINT("it sees the thread's TPIDRURO", cb_ro == thread_ro);
    PRINT("after the wait: TPIDRURW", after_cb);
    PRINT("after the wait: its TPIDRURO is the same", ro_after_cb == thread_ro);
    PRINT("a callback run by a check sees TPIDRURW", check_cb_rw);
    PRINT("it sees the thread's TPIDRURO", check_cb_ro == thread_ro);
    PRINT("after the check: TPIDRURW", after_check);
    PRINT("after the check: its TPIDRURO is the same", ro_after_check == thread_ro);

    sceKernelStartThread(thread, 0, NULL);
    test_join_thread(thread);
    PRINT("started again: TPIDRURW", restart_rw);
    PRINT("started again: its TPIDRURO is the same", restart_ro == thread_ro);

    test_exit();
    return 0;
}
