// What a condition variable waiter W gets when its condition or its mutex is deleted after a
// signal woke it, for a heavy condition (sceKernelCreateCond) and a lightweight one
// (sceKernelCreateLwCond). Each case runs on fresh objects. Every thread is pinned to core 0. W
// runs at 0xB0, below the main thread at 0x60, so a thread the main thread wakes stays ready and
// doesn't run until the main thread blocks. W locks the mutex and waits on the condition with a
// timeout of 6 s. Once the wait returns it records the result and whether it owns the mutex, then
// unlocks the mutex and records that result. The cases:
// - the main thread locks the mutex and signals, so W waits to lock the mutex again, then deletes
//   the condition while W waits, and unlocks the mutex
// - the main thread signals and deletes the condition before W runs
// - the main thread locks the mutex, signals, and deletes the mutex before W runs
// Each case prints whether W waited where the case needs it (on the condition, then for the mutex
// in the first case, or still in its wait call after the signal in the others), the delete's
// result, W's wait result, whether W owned the mutex after the wait (not in the last case, where
// the mutex is gone), and W's unlock result. The last two cases also print whether W's wait
// returned within 5 s of the delete, and print the rest only if it did. The first case also
// prints the wait type W reported while it waited for the mutex, and whether its wait UID was the
// mutex's.
//
// On a PS Vita TV on firmware 3.65 W waits for the mutex with the wait type 0x200 or 0x400000 and
// the mutex's UID, and the delete of the condition doesn't end that wait, so W gets 0 with the
// mutex for both kinds. A condition deleted after the signal but before W runs gives W 0 without
// the mutex, and its unlock fails with 0x80028146 or 0x80028186. A heavy mutex deleted the same way
// gives W 0x80028141. A lightweight one leaves W blocked, and it doesn't return within 5 s.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xB0
#define WAIT_TIMEOUT_US (3 * TEST_WAIT_US)
#define NOT_SET 0xDEAD

// One kind of condition variable, with the calls that differ
typedef struct Kind {
    const char *name;
    SceUInt32 cond_wait_type;
    SceUInt32 mutex_wait_type;
    void (*create)(void);
    void (*destroy)(void);
    int (*lock)(void);
    int (*unlock)(void);
    int (*wait)(SceUInt *timeout);
    int (*signal)(void);
    int (*delete_cond)(void);
    int (*delete_mutex)(void);
    SceUID (*owner)(void);
} Kind;

static SceUID mutex, cond;
static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
// The UIDs a wait reports, which the create calls store
static SceUID cond_id, mutex_id;

static void heavy_create(void) {
    mutex = sceKernelCreateMutex("mutex", SCE_KERNEL_ATTR_THREAD_PRIO, 0, NULL);
    cond = sceKernelCreateCond("cond", SCE_KERNEL_ATTR_THREAD_PRIO, mutex, NULL);
    mutex_id = mutex;
    cond_id = cond;
}

static void heavy_destroy(void) {
    sceKernelDeleteCond(cond);
    sceKernelDeleteMutex(mutex);
}

static int heavy_lock(void) {
    return sceKernelLockMutex(mutex, 1, NULL);
}

static int heavy_unlock(void) {
    return sceKernelUnlockMutex(mutex, 1);
}

static int heavy_wait(SceUInt *timeout) {
    return sceKernelWaitCond(cond, timeout);
}

static int heavy_signal(void) {
    return sceKernelSignalCond(cond);
}

static int heavy_delete_cond(void) {
    return sceKernelDeleteCond(cond);
}

static int heavy_delete_mutex(void) {
    return sceKernelDeleteMutex(mutex);
}

static SceUID heavy_owner(void) {
    MutexInfo info = { .size = sizeof(info) };
    if (sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info) < 0)
        return 0;
    return info.currentOwnerId;
}

static void lw_create(void) {
    sceKernelCreateLwMutex(&lw_mutex, "lw mutex", SCE_KERNEL_ATTR_THREAD_PRIO, 0, NULL);
    sceKernelCreateLwCond(&lw_cond, "lw cond", SCE_KERNEL_ATTR_THREAD_PRIO, &lw_mutex, NULL);
    LwMutexInfo mutex_info = { .size = sizeof(mutex_info) };
    LwCondInfo cond_info = { .size = sizeof(cond_info) };
    sceKernelGetLwMutexInfo(&lw_mutex, &mutex_info);
    sceKernelGetLwCondInfo(&lw_cond, &cond_info);
    mutex_id = mutex_info.uid;
    cond_id = cond_info.uid;
}

static void lw_destroy(void) {
    sceKernelDeleteLwCond(&lw_cond);
    sceKernelDeleteLwMutex(&lw_mutex);
}

static int lw_lock(void) {
    return sceKernelLockLwMutex(&lw_mutex, 1, NULL);
}

static int lw_unlock(void) {
    return sceKernelUnlockLwMutex(&lw_mutex, 1);
}

static int lw_wait(SceUInt *timeout) {
    return sceKernelWaitLwCond(&lw_cond, timeout);
}

static int lw_signal(void) {
    return sceKernelSignalLwCond(&lw_cond);
}

static int lw_delete_cond(void) {
    return sceKernelDeleteLwCond(&lw_cond);
}

static int lw_delete_mutex(void) {
    return sceKernelDeleteLwMutex(&lw_mutex);
}

static SceUID lw_owner(void) {
    LwMutexInfo info = { .size = sizeof(info) };
    if (sceKernelGetLwMutexInfo(&lw_mutex, &info) < 0)
        return 0;
    return info.currentOwnerId;
}

static const Kind kinds[2] = {
    { "heavy", WAIT_TYPE_COND, WAIT_TYPE_COND_MUTEX, heavy_create, heavy_destroy, heavy_lock,
        heavy_unlock, heavy_wait, heavy_signal, heavy_delete_cond, heavy_delete_mutex, heavy_owner },
    { "lightweight", WAIT_TYPE_LW_COND, WAIT_TYPE_LW_COND_MUTEX, lw_create, lw_destroy, lw_lock,
        lw_unlock, lw_wait, lw_signal, lw_delete_cond, lw_delete_mutex, lw_owner },
};

static const Kind *kind;
static volatile int read_owner, w_result, w_holds, w_unlock, w_done;

// Locks the mutex, waits on the condition, then records the result, the mutex's owner and the
// result of an unlock
static int w_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    kind->lock();
    SceUInt timeout = WAIT_TIMEOUT_US;
    w_result = kind->wait(&timeout);
    if (read_owner)
        w_holds = kind->owner() == sceKernelGetThreadId();
    w_unlock = kind->unlock();
    w_done = 1;
    return 0;
}

static SceUInt32 wait_type_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (sceKernelGetThreadInfo(thread, &info) < 0 || !(info.status & SCE_THREAD_WAITING))
        return 0;
    return info.waitType;
}

static int wait_id_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (sceKernelGetThreadInfo(thread, &info) < 0)
        return 0;
    return info.waitId;
}

// Creates the objects and W, and waits until W waits on the condition
static SceUID start_waiter(const Kind *k, int with_owner) {
    kind = k;
    read_owner = with_owner;
    w_result = w_holds = w_unlock = NOT_SET;
    w_done = 0;
    k->create();
    const SceUID w = sceKernelCreateThread(
        "waiter", w_entry, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(w, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(w, k->cond_wait_type, cond_id));
    return w;
}

// Waits until W has recorded its results, ends it, and deletes what is left of the objects. A W
// that is still blocked after that is left as it is. Returns whether W recorded its results.
static int end_waiter(SceUID w) {
    TEST_WAIT_FOR(w_done);
    const int done = w_done;
    SceUInt timeout = TEST_WAIT_US;
    if (done && sceKernelWaitThreadEnd(w, NULL, &timeout) == 0)
        sceKernelDeleteThread(w);
    kind->destroy();
    return done;
}

// The main thread holds the mutex and signals, so W waits to lock it again, then the condition is
// deleted
static void delete_cond_relocking(const Kind *k) {
    const char *const name = "delete the condition while W waits for the mutex";
    const SceUID w = start_waiter(k, 1);
    const int waits_on_cond = test_in_wait(w, k->cond_wait_type, cond_id);
    k->lock();
    k->signal();
    TEST_WAIT_FOR(wait_type_of(w) == k->mutex_wait_type);
    const SceUInt32 type = wait_type_of(w);
    const int id_is_mutex = wait_id_of(w) == mutex_id;
    const int deleted = k->delete_cond();
    k->unlock();
    end_waiter(w);
    PRINTF(waits_on_cond, "%s, %s: W waits on the condition", k->name, name);
    PRINTF(type, "%s, %s: W's wait type for the mutex", k->name, name);
    PRINTF(id_is_mutex, "%s, %s: the wait UID is the mutex's", k->name, name);
    PRINTF(deleted, "%s, %s: delete the condition", k->name, name);
    PRINTF(w_result, "%s, %s: W's result", k->name, name);
    PRINTF(w_holds, "%s, %s: W owns the mutex after it", k->name, name);
    PRINTF(w_unlock, "%s, %s: W's unlock after it", k->name, name);
}

// The main thread signals, then deletes the condition or the mutex with no call that lets W run
// in between. For the mutex it holds the mutex first.
static void signal_then_delete(const Kind *k, int delete_the_mutex) {
    const char *const name = delete_the_mutex ? "lock, signal, then delete the mutex before W runs"
                                              : "signal, then delete the condition before W runs";
    const SceUID w = start_waiter(k, !delete_the_mutex);
    const int waits_on_cond = test_in_wait(w, k->cond_wait_type, cond_id);
    // A held mutex makes a thread that gets to lock it wait, so a thread that doesn't follow the
    // core and priority order can't take the mutex before the delete
    if (delete_the_mutex)
        k->lock();
    k->signal();
    const int not_back = w_result == NOT_SET;
    const int deleted = delete_the_mutex ? k->delete_mutex() : k->delete_cond();
    const int returned = end_waiter(w);
    PRINTF(waits_on_cond, "%s, %s: W waits on the condition", k->name, name);
    PRINTF(not_back, "%s, %s: W is still in its wait after the signal", k->name, name);
    PRINTF(deleted, "%s, %s: the delete", k->name, name);
    PRINTF(returned, "%s, %s: W's wait returned within 5 s", k->name, name);
    if (!returned)
        return;
    PRINTF(w_result, "%s, %s: W's result", k->name, name);
    if (!delete_the_mutex)
        PRINTF(w_holds, "%s, %s: W owns the mutex after it", k->name, name);
    PRINTF(w_unlock, "%s, %s: W's unlock after it", k->name, name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    for (int i = 0; i < 2; i++)
        delete_cond_relocking(&kinds[i]);
    for (int i = 0; i < 2; i++)
        signal_then_delete(&kinds[i], 0);
    for (int i = 0; i < 2; i++)
        signal_then_delete(&kinds[i], 1);

    test_exit();
    return 0;
}
