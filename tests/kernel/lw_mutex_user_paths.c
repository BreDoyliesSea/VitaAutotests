// Lightweight mutex corner cases on the user side of the calls, where the work area decides the
// result before the system is asked. The cases:
// - the try lock of a count of 2 on a mutex without the recursive attribute, while it is free,
//   while the caller holds it and while another thread holds it
// - the try lock of a recursive mutex whose count wrapped, by its owner and by another thread
// - the owner's unlock after the mutex was deleted while held, which words of the work area it
//   leaves, and a lock of the deleted work area
// - the unlock of a work area with a bad UID word, a zeroed one and one whose word 4 is -1, through
//   both unlock calls
// - the info by UID of an unknown UID, with a size past the struct and with no struct
//
// On a PS Vita TV on firmware 3.65 the try lock of a count of 2 gives 0x8002000F while the mutex is
// free, 0x80028182 while the caller holds it and 0x80028185 while another thread does. After the
// wrap the owner's try lock gives 0x80028183 and the other thread's gives 0x80028185. The unlock
// after the delete works and clears words 0 to 2 of the work area, and the lock of that work area
// works again. Both unlocks of a zeroed work area refuse with different errors, 0x80028181 for
// sceKernelUnlockLwMutex and 0x8002000F for sceKernelUnlockLwMutex2, and a UID word of -1 gives
// 0x80028181 for both. The info with a size of 0x44 gives 0x8002710B before the UID is looked up,
// and the info with no struct gives the unknown UID error 0x80028181.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceKernelLwMutexWork *other_work;
static int other_count;
static SceUID gate;
static volatile int other_result;

// Locks other_work once and keeps it until the gate is signaled
static int holder_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = 3 * TEST_WAIT_US;
    sceKernelLockLwMutex(other_work, 1, NULL);
    sceKernelWaitSema(gate, 1, &timeout);
    sceKernelUnlockLwMutex(other_work, 1);
    return 0;
}

// Tries to lock other_work with other_count and records the result
static int try_lock_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    other_result = sceKernelTryLockLwMutex(other_work, other_count);
    return 0;
}

static void print_work_words(const char *name, const SceKernelLwMutexWork *work) {
    const volatile unsigned int *words = (const volatile unsigned int *)work;
    for (int i = 0; i < 3; i++)
        PRINTF(words[i] != 0, "%s: word %d is not 0", name, i);
}

int main(void) {
    // The try lock checks the count after it looks at the owner
    static SceKernelLwMutexWork mutex;
    sceKernelCreateLwMutex(&mutex, "mutex", 0, 0, NULL);
    PRINT("try lock of 2 while free", sceKernelTryLockLwMutex(&mutex, 2));
    sceKernelLockLwMutex(&mutex, 1, NULL);
    PRINT("try lock of 2 while the caller holds it", sceKernelTryLockLwMutex(&mutex, 2));
    sceKernelUnlockLwMutex(&mutex, 1);
    sceKernelDeleteLwMutex(&mutex);

    static SceKernelLwMutexWork held;
    sceKernelCreateLwMutex(&held, "held", 0, 0, NULL);
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);
    other_work = &held;
    const SceUID holder = test_start_thread(holder_entry);
    TEST_WAIT_FOR(test_in_wait(holder, WAIT_TYPE_SEMA, gate));
    PRINT("try lock of 2 while another thread holds it", sceKernelTryLockLwMutex(&held, 2));
    sceKernelSignalSema(gate, 1);
    test_join_thread(holder);
    sceKernelDeleteLwMutex(&held);

    // The count of a recursive mutex wraps the way lw_mutex shows
    static SceKernelLwMutexWork wrapped;
    sceKernelCreateLwMutex(&wrapped, "wrapped", LW_MUTEX_ATTR_RECURSIVE, 0, NULL);
    sceKernelLockLwMutex(&wrapped, 2, NULL);
    sceKernelLockLwMutex(&wrapped, 1, NULL);
    PRINT("the lock that wraps the count", sceKernelLockLwMutex(&wrapped, 0x7FFFFFFF, NULL));
    PRINT("try lock by the owner after the wrap", sceKernelTryLockLwMutex(&wrapped, 1));
    other_work = &wrapped;
    other_count = 1;
    test_join_thread(test_start_thread(try_lock_entry));
    PRINT("try lock by another thread after the wrap", other_result);
    sceKernelDeleteLwMutex(&wrapped);

    // The owner deletes the mutex while it holds it
    static SceKernelLwMutexWork deleted;
    sceKernelCreateLwMutex(&deleted, "deleted", 0, 0, NULL);
    sceKernelLockLwMutex(&deleted, 1, NULL);
    PRINT("delete while held", sceKernelDeleteLwMutex(&deleted));
    PRINT("unlock by the owner after the delete", sceKernelUnlockLwMutex(&deleted, 1));
    print_work_words("the deleted work area after the unlock", &deleted);
    SceUInt32 timeout = 0;
    PRINT("lock of the deleted work area", sceKernelLockLwMutex(&deleted, 1, &timeout));

    // Work areas whose UID word isn't one of a mutex
    static SceKernelLwMutexWork zeroed, negative;
    sceClibMemset(&zeroed, 0, sizeof(zeroed));
    sceClibMemset(&negative, 0, sizeof(negative));
    ((volatile unsigned int *)&negative)[4] = 0xFFFFFFFF;
    PRINT("unlock of 1 with a UID word of 0", sceKernelUnlockLwMutex(&zeroed, 1));
    PRINT("unlock 2 of 1 with a UID word of 0", sceKernelUnlockLwMutex2(&zeroed, 1));
    PRINT("unlock of 1 with a UID word of -1", sceKernelUnlockLwMutex(&negative, 1));
    PRINT("unlock 2 of 1 with a UID word of -1", sceKernelUnlockLwMutex2(&negative, 1));

    // The unknown UID, with room behind the struct in case the call writes as much as the size says
    static union {
        LwMutexInfo info;
        unsigned char bytes[sizeof(LwMutexInfo) + 64];
    } big;
    sceClibMemset(&big, 0xEE, sizeof(big));
    big.info.size = 0x44;
    PRINT("info by an unknown UID with a size of 0x44", sceKernelGetLwMutexInfoById(0x12345, &big.info));

    // NULL pointers last, so an implementation that crashes on them loses nothing else
    PRINT("info by an unknown UID with no struct", sceKernelGetLwMutexInfoById(0x12345, NULL));

    test_exit();
    return 0;
}
