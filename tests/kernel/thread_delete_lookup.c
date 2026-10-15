// Whether a thread can still be found right after it's deleted. Right after each delete, before
// anything prints, the test reads what sceKernelGetThreadInfo, sceKernelGetThreadExitStatus and
// sceKernelDeleteThread return for it, then prints them:
// - a thread that never ran, deleted by sceKernelDeleteThread
// - a thread that ran and ended, deleted by sceKernelDeleteThread
// - a thread that exits and deletes itself, read by a thread whose sceKernelWaitThreadEnd on it
//   was queued before it got go, with what the end wait returns and the exit status it gets
// Then it prints, for 20 threads that exit and delete themselves the same way, how many end waits
// returned 0 and how many of the threads could still be found right after. Last, 10 times, a
// thread on core 1 waits for go while the main thread, on core 0, reads its info in a loop, sends
// go after the first read that finds it, and reads on until it can't find it. The test prints how
// many loops ended that way, what the last read of the last loop returned, how many loops had a
// read that saw the thread DORMANT, and whether every thread got its go.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID go;
// How many threads that waited for go didn't get it
static int missed_go;

typedef struct Lookups {
    int info, exit_status, delete_again;
} Lookups;

static int returns_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 5;
}

// Exits and deletes itself once it gets go
static int exit_deletes_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    if (sceKernelWaitSema(go, 1, &timeout) != 0)
        __atomic_fetch_add(&missed_go, 1, __ATOMIC_SEQ_CST);
    sceKernelExitDeleteThread(7);
    return 0;
}

static SceUID create(SceKernelThreadEntry entry, SceUInt32 mask) {
    return sceKernelCreateThread("t", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, mask, NULL);
}

// Looks thread up three ways in a row
static Lookups look_up(SceUID thread) {
    Lookups lookups;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    lookups.info = sceKernelGetThreadInfo(thread, &info);
    int status = 0xDEAD;
    lookups.exit_status = sceKernelGetThreadExitStatus(thread, &status);
    lookups.delete_again = sceKernelDeleteThread(thread);
    return lookups;
}

static void print_lookups(const char *name, Lookups lookups) {
    PRINTF(lookups.info, "%s: its info right after", name);
    PRINTF(lookups.exit_status, "%s: its exit status right after", name);
    PRINTF(lookups.delete_again, "%s: delete it again right after", name);
}

static SceUID end_target;
static volatile int end_result, end_status;
static Lookups end_lookups;

// Waits for the end of end_target and, if the wait ended it, looks it up right after
static int end_waiter_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    int status = 0xDEAD;
    SceUInt timeout = TEST_WAIT_US;
    end_result = sceKernelWaitThreadEnd(end_target, &status, &timeout);
    if (end_result == 0)
        end_lookups = look_up(end_target);
    else
        end_lookups = (Lookups){ 0xDEAD, 0xDEAD, 0xDEAD };
    end_status = status;
    return 0;
}

// Starts a thread that exits and deletes itself once a waiter for its end is queued
static void exit_delete_with_end_waiter(void) {
    end_target = create(exit_deletes_entry, 0);
    sceKernelStartThread(end_target, 0, NULL);
    test_wait_until_waiting(end_target);
    const SceUID waiter = test_start_thread(end_waiter_entry);
    test_wait_until_waiting(waiter);
    sceKernelSignalSema(go, 1);
    test_join_thread(waiter);
}

int main(void) {
    go = sceKernelCreateSema("go", 0, 0, 1, NULL);

    SceUID thread = create(returns_entry, 0);
    int deleted = sceKernelDeleteThread(thread);
    Lookups lookups = look_up(thread);
    PRINT("never ran: delete it", deleted);
    print_lookups("never ran", lookups);

    thread = create(returns_entry, 0);
    sceKernelStartThread(thread, 0, NULL);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    deleted = sceKernelDeleteThread(thread);
    lookups = look_up(thread);
    PRINT("ran and ended: delete it", deleted);
    print_lookups("ran and ended", lookups);

    exit_delete_with_end_waiter();
    PRINT("exits and deletes itself: the end wait", end_result);
    PRINT("exits and deletes itself: the status it gets", end_status);
    print_lookups("exits and deletes itself", end_lookups);

    int ended = 0, found = 0;
    for (int i = 0; i < 20; i++) {
        exit_delete_with_end_waiter();
        ended += end_result == 0;
        found += end_result == 0 && end_lookups.info >= 0;
    }
    PRINT("20 that exit and delete themselves: how many end waits returned 0", ended);
    PRINT("20 that exit and delete themselves: how many are found right after their end waits", found);

    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);
    int ended_on_failed_read = 0, saw_alive_first = 0, saw_dormant = 0, last = 0;
    for (int round = 0; round < 10; round++) {
        thread = create(exit_deletes_entry, SCE_KERNEL_CPU_MASK_USER_1);
        sceKernelStartThread(thread, 0, NULL);
        test_wait_until_waiting(thread);
        int read = 0, released = 0, dormant = 0;
        const SceUInt64 end = sceKernelGetProcessTimeWide() + TEST_WAIT_US;
        // Reads as fast as it can, with no pause, since a state on the way to deletion may only last
        // an instant
        while (sceKernelGetProcessTimeWide() < end) {
            SceKernelThreadInfo info = { .size = sizeof(info) };
            read = sceKernelGetThreadInfo(thread, &info);
            if (read < 0)
                break;
            dormant |= info.status == SCE_THREAD_DORMANT;
            // Only a thread seen alive may end
            if (!released) {
                sceKernelSignalSema(go, 1);
                released = 1;
            }
        }
        // A thread still there at the time limit ends before the next round
        if (read >= 0) {
            SceUInt timeout = TEST_WAIT_US;
            sceKernelWaitThreadEnd(thread, NULL, &timeout);
        }
        ended_on_failed_read += read < 0;
        saw_alive_first += released;
        saw_dormant += dormant;
        last = read;
    }
    PRINT("10 read loops while a thread exits and deletes itself: how many ended on a failed read", ended_on_failed_read);
    PRINT("10 read loops while a thread exits and deletes itself: how many found it alive first", saw_alive_first);
    PRINT("10 read loops while a thread exits and deletes itself: the last read", last);
    PRINT("10 read loops while a thread exits and deletes itself: how many saw it DORMANT", saw_dormant);
    PRINT("every thread that waited for go got it", __atomic_load_n(&missed_go, __ATOMIC_SEQ_CST) == 0);

    sceKernelDeleteSema(go);
    test_exit();
    return 0;
}
