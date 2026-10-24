// The same simple event listed twice in sceKernelWaitMultipleEvents, and what each of its entries
// gets. A waiter thread makes the call where the case needs one. Cases:
// - OR on the auto reset event A twice, set once while the call waits
// - OR on A twice with A already set before the call, with A manual reset, then auto reset
// - priority: thread M at 0x90 waits with AND on the auto reset event A twice, then a single waiter
//   S at 0x80 waits for A, then M is raised to 0x70 and A is set twice, one set at a time. A has
//   SCE_KERNEL_ATTR_THREAD_PRIO, so its queue is ordered by priority. The case prints whether each
//   of M and S still waits after each set and the bits of A, then the results of both once a cancel
//   of A ended whatever still waited.
// Every thread of the priority case runs on core 0 below the main thread, which also stays on core
// 0, so a thread that a set woke is still in no wait when the main thread looks at it. The case
// prints only after its cancel, since a print lets the threads of core 0 run.
//
// On a PS Vita TV on firmware 3.65 the OR wait on A twice returns 1 entry, with the user data of
// the set, and A's bit is consumed once. With A already set before the call, a manual reset A
// gives both entries and keeps its bit, while an auto reset A gives the first entry only, since
// that entry consumes the bit. In the priority case the raise moves both entries of M in the
// queue of A, so the first set goes to M, which still waits for its second entry, and the second
// set ends M's wait with 2 entries, the first with the user data of the first set and the second
// with the user data of the second. S keeps waiting until the cancel, which reports 1 waiter and
// ends S with 0x80028007.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define M_PRIORITY 0x90
#define M_RAISED_PRIORITY 0x70
#define S_PRIORITY 0x80
#define NOT_DONE 0xDEAD

static SceUID a;
static WaitEvent list[2];
static ResultEvent results[2];
static SceUInt32 wait_mode;
static volatile int wait_result, single_result;
static SceUInt32 single_pattern;
static SceUInt64 single_user_data;

// Waits on the list with the mode of the case
static int multi_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = 2 * TEST_WAIT_US;
    wait_result = sceKernelWaitMultipleEvents(list, 2, wait_mode, results, &timeout);
    return 0;
}

// Waits for EVENT_BIT_A of A alone
static int single_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = 2 * TEST_WAIT_US;
    single_result = sceKernelWaitEvent(a, EVENT_BIT_A, &single_pattern, &single_user_data, &timeout);
    return 0;
}

// Returns whether thread is in a wait of the given type
static int in_wait(SceUID thread, SceUInt32 type) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && info.waitType == type;
}

// Starts entry on core 0 at the priority, and returns once the thread is in a wait of the type
static SceUID start_waiter(SceKernelThreadEntry entry, int priority, SceUInt32 type) {
    const SceUID thread = sceKernelCreateThread("waiter", entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(in_wait(thread, type));
    return thread;
}

// Lists A twice, each waiting for EVENT_BIT_A, with a marker in the results
static void setup(SceUInt32 attr, SceUInt32 mode) {
    a = sceKernelCreateSimpleEvent("a", attr, 0, NULL);
    list[0] = list[1] = (WaitEvent){ a, EVENT_BIT_A };
    wait_mode = mode;
    wait_result = single_result = NOT_DONE;
    single_pattern = 0xDEAD;
    single_user_data = 0xDEAD;
    sceClibMemset(results, 0xAA, sizeof(results));
}

static SceUInt32 bits_of(SceUID event) {
    SceUInt32 pattern = 0xDEAD;
    sceKernelGetEventPattern(event, &pattern);
    return pattern;
}

// Prints the entries a call that returned count filled
static void print_entries(const char *name, int count) {
    for (int i = 0; i < 2 && i < count; i++) {
        PRINTF(results[i].eventId == a, "%s: entry %d is A", name, i);
        PRINTF(results[i].result, "%s: entry %d result", name, i);
        PRINTF(results[i].resultPattern, "%s: entry %d bits", name, i);
        PRINTF((int)results[i].userData, "%s: entry %d user data", name, i);
    }
}

// OR on A twice with A set once while the call waits
static void or_set_while_waiting(void) {
    const char *name = "OR, A twice, auto reset, set while waiting";
    setup(EVENT_ATTR_AUTO_RESET, EVENT_WAIT_MODE_OR);
    const SceUID waiter = start_waiter(multi_waiter, M_PRIORITY, WAIT_TYPE_MP_EVENTS);
    PRINTF(sceKernelSetEvent(a, EVENT_BIT_A, 0x77), "%s: the set", name);
    test_join_thread(waiter);
    PRINTF(wait_result, "%s: the call", name);
    print_entries(name, wait_result);
    PRINTF(bits_of(a), "%s: the bits of A after", name);
    sceKernelDeleteSimpleEvent(a);
}

// OR on A twice with A already set before the call
static void or_set_before(const char *name, SceUInt32 attr) {
    setup(attr, EVENT_WAIT_MODE_OR);
    sceKernelSetEvent(a, EVENT_BIT_A, 0x77);
    SceUInt32 timeout = 1000;
    const int count = sceKernelWaitMultipleEvents(list, 2, EVENT_WAIT_MODE_OR, results, &timeout);
    PRINTF(count, "%s: the call", name);
    print_entries(name, count);
    PRINTF(bits_of(a), "%s: the bits of A after", name);
    sceKernelDeleteSimpleEvent(a);
}

// M waits with AND on A twice, S waits on A alone, M is raised above S, then A is set twice
static void priority_case(void) {
    const char *name = "AND, A twice, M raised above S";
    setup(SCE_KERNEL_ATTR_THREAD_PRIO | EVENT_ATTR_AUTO_RESET, EVENT_WAIT_MODE_AND);
    const SceUID m = start_waiter(multi_waiter, M_PRIORITY, WAIT_TYPE_MP_EVENTS);
    const SceUID s = start_waiter(single_waiter, S_PRIORITY, WAIT_TYPE_EVENT);

    // Nothing is printed until the cancel ended every wait
    const int raise = sceKernelChangeThreadPriority(m, M_RAISED_PRIORITY);
    sceKernelSetEvent(a, EVENT_BIT_A, 0x77);
    const int s_waits_1 = in_wait(s, WAIT_TYPE_EVENT);
    const int m_waits_1 = in_wait(m, WAIT_TYPE_MP_EVENTS);
    const SceUInt32 bits_1 = bits_of(a);
    sceKernelSetEvent(a, EVENT_BIT_A, 0x88);
    const int s_waits_2 = in_wait(s, WAIT_TYPE_EVENT);
    const int m_waits_2 = in_wait(m, WAIT_TYPE_MP_EVENTS);
    const SceUInt32 bits_2 = bits_of(a);
    SceUInt32 waiters = 0xDEAD;
    const int cancel = sceKernelCancelEvent(a, &waiters);

    test_join_thread(m);
    test_join_thread(s);
    PRINTF(raise, "%s: raise M", name);
    PRINTF(s_waits_1, "%s: S still waits after the first set", name);
    PRINTF(m_waits_1, "%s: M still waits after the first set", name);
    PRINTF(bits_1, "%s: the bits of A after the first set", name);
    PRINTF(s_waits_2, "%s: S still waits after the second set", name);
    PRINTF(m_waits_2, "%s: M still waits after the second set", name);
    PRINTF(bits_2, "%s: the bits of A after the second set", name);
    PRINTF(cancel, "%s: the cancel", name);
    PRINTF(waiters, "%s: the waiters the cancel reports", name);
    PRINTF(wait_result, "%s: M's call", name);
    print_entries(name, wait_result);
    PRINTF(single_result, "%s: S's wait", name);
    if (single_result == 0)
        PRINTF((int)single_user_data, "%s: S's user data", name);
    sceKernelDeleteSimpleEvent(a);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    or_set_while_waiting();
    or_set_before("OR, A twice, set before", EVENT_ATTR_MANUAL_RESET);
    or_set_before("OR, A twice, auto reset, set before", EVENT_ATTR_AUTO_RESET);
    // Last, since a wrong queue order can leave a wait for the cancel to end
    priority_case();

    test_exit();
    return 0;
}
