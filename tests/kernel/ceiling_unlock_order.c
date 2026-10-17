// What a thread's priority is after each step of locking and unlocking mutexes with a ceiling, in
// the cases mutex_ceiling leaves out. Owner O runs at 0xB0 on core 0, runs the steps of a case,
// and notes each step's result and its priority after it, while the main thread runs on core 1.
// The output calls a mutex by its ceiling, like ceiling 0x80. The cases:
// - ceilings 0x80 and 0x70, locked in that order and unlocked in the same order
// - ceilings 0x70 and 0x80, locked in that order and unlocked in the same order
// - two mutexes with the ceiling 0x80, both locked, then unlocked
// - a recursive mutex with the ceiling 0x80, locked twice, then unlocked twice
// - the same, locked once with a count of 2, then unlocked one count at a time
// - a try lock of a mutex with the ceiling 0x80, then its unlock
// - O deletes a mutex with the ceiling 0x80 that it holds
// - O at 0x70 locks a mutex with the ceiling 0x80, lowers itself to 0xB0, then unlocks
//
// On a PS Vita TV on firmware 3.65 O runs at the highest of its priority and the ceilings it holds,
// in any lock and unlock order: with 0x80 and 0x70 held it runs at 0x70, back at 0x80 or 0x70 once
// one goes and at 0xB0 once both do. Two 0x80 mutexes keep 0x80 until both are unlocked, a
// recursive one until its last unlock, whether locked twice or with a count of 2, and a try lock
// boosts too. Deleting a held ceiling mutex drops its boost at once. O at 0x70 holding a 0x80 that
// lowers itself to 0xB0 keeps 0x70, not the ceiling, until the unlock, which gives it 0xB0.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define OWNER_PRIORITY 0xB0
#define MAX_STEPS 4

enum Op { END, LOCK, LOCK_2, TRY_LOCK, UNLOCK, DELETE, SET_PRIORITY };

typedef struct Step {
    int op, mutex, arg;
} Step;

typedef struct Case {
    const char *name;
    int owner_priority, ceilings[2], recursive;
    Step steps[MAX_STEPS];
} Case;

static const Case cases[] = {
    { "ceilings 0x80 then 0x70, unlocked in that order", OWNER_PRIORITY, { 0x80, 0x70 }, 0,
      { { LOCK, 0, 0 }, { LOCK, 1, 0 }, { UNLOCK, 0, 0 }, { UNLOCK, 1, 0 } } },
    { "ceilings 0x70 then 0x80, unlocked in that order", OWNER_PRIORITY, { 0x70, 0x80 }, 0,
      { { LOCK, 0, 0 }, { LOCK, 1, 0 }, { UNLOCK, 0, 0 }, { UNLOCK, 1, 0 } } },
    { "two ceilings 0x80", OWNER_PRIORITY, { 0x80, 0x80 }, 0,
      { { LOCK, 0, 0 }, { LOCK, 1, 0 }, { UNLOCK, 0, 0 }, { UNLOCK, 1, 0 } } },
    { "a recursive ceiling 0x80, locked twice", OWNER_PRIORITY, { 0x80, 0 }, 1,
      { { LOCK, 0, 0 }, { LOCK, 0, 0 }, { UNLOCK, 0, 0 }, { UNLOCK, 0, 0 } } },
    { "a recursive ceiling 0x80, locked with a count of 2", OWNER_PRIORITY,
      { 0x80, 0 }, 1, { { LOCK_2, 0, 0 }, { UNLOCK, 0, 0 }, { UNLOCK, 0, 0 }, { END, 0, 0 } } },
    { "a try lock of a ceiling 0x80", OWNER_PRIORITY, { 0x80, 0 }, 0,
      { { TRY_LOCK, 0, 0 }, { UNLOCK, 0, 0 }, { END, 0, 0 } } },
    { "O deletes a ceiling 0x80 it holds", OWNER_PRIORITY, { 0x80, 0 }, 0,
      { { LOCK, 0, 0 }, { DELETE, 0, 0 }, { END, 0, 0 } } },
    { "O at 0x70 holds a ceiling 0x80, lowers itself to 0xB0", 0x70, { 0x80, 0 }, 0,
      { { LOCK, 0, 0 }, { SET_PRIORITY, 0, 0xB0 }, { UNLOCK, 0, 0 }, { END, 0, 0 } } },
};

// What each step does, given the ceiling of its mutex or the priority it sets
static const char *const op_names[] = {
    "",          "lock 0x%X",   "lock 0x%X with a count of 2", "try lock 0x%X",
    "unlock 0x%X", "delete 0x%X", "set its priority to 0x%X",
};

static const Case *current;
static SceUID mutexes[2];
static int results[MAX_STEPS], priorities[MAX_STEPS], deleted[2];

static int owner_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (int i = 0; i < MAX_STEPS && current->steps[i].op != END; i++) {
        const Step *step = &current->steps[i];
        const SceUID mutex = mutexes[step->mutex];
        int result = 0;
        if (step->op == LOCK) {
            result = sceKernelLockMutex(mutex, 1, NULL);
        } else if (step->op == LOCK_2) {
            result = sceKernelLockMutex(mutex, 2, NULL);
        } else if (step->op == TRY_LOCK) {
            result = sceKernelTryLockMutex(mutex, 1);
        } else if (step->op == UNLOCK) {
            result = sceKernelUnlockMutex(mutex, 1);
        } else if (step->op == DELETE) {
            result = sceKernelDeleteMutex(mutex);
            deleted[step->mutex] = result == 0;
        } else {
            result = sceKernelChangeThreadPriority(0, step->arg);
        }
        results[i] = result;
        priorities[i] = sceKernelGetThreadCurrentPriority();
    }
    return 0;
}

static void run_case(const Case *c) {
    current = c;
    for (int i = 0; i < 2; i++) {
        deleted[i] = 0;
        mutexes[i] = -1;
        if (c->ceilings[i]) {
            SceKernelMutexOptParam opt = { .size = sizeof(opt), .ceilingPriority = c->ceilings[i] };
            const SceUInt32 attr = SCE_KERNEL_MUTEX_ATTR_CEILING |
                                   (c->recursive ? SCE_KERNEL_MUTEX_ATTR_RECURSIVE : 0);
            mutexes[i] = sceKernelCreateMutex("ceiling", attr, 0, &opt);
        }
    }
    for (int i = 0; i < MAX_STEPS; i++)
        results[i] = priorities[i] = -1;
    const SceUID owner = sceKernelCreateThread("owner", owner_entry, c->owner_priority, 0x4000, 0,
                                               SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(owner, 0, NULL);
    test_join_thread(owner);
    for (int i = 0; i < 2; i++) {
        if (mutexes[i] >= 0 && !deleted[i])
            sceKernelDeleteMutex(mutexes[i]);
    }

    for (int i = 0; i < MAX_STEPS && c->steps[i].op != END; i++) {
        const Step *step = &c->steps[i];
        char what[64];
        const int arg = step->op == SET_PRIORITY ? step->arg : c->ceilings[step->mutex];
        char step_name[40];
        sceClibSnprintf(step_name, sizeof(step_name), op_names[step->op], arg);
        sceClibSnprintf(what, sizeof(what), "step %d, %s", i + 1, step_name);
        PRINTF(results[i], "%s, %s: the result", c->name, what);
        PRINTF(priorities[i], "%s, %s: the priority after it", c->name, what);
    }
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);
    test_exit();
    return 0;
}
