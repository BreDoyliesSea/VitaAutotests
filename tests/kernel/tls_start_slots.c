// What the 256 TLS slots of a new thread and of the main thread hold before the test writes any:
// which keys aren't 0, and whether each value is the thread's UID, the low or high end of its
// stack, an address on its stack, its priority, its affinity, the process ID, or something else.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define SLOTS 256

enum { OTHER = 1, THREAD_UID, STACK_LOW, STACK_HIGH, ON_STACK, PRIORITY, AFFINITY, PROCESS_ID };

static SceUInt32 slots[SLOTS];
static SceKernelThreadInfo info;

static void read_slots(void) {
    for (int i = 0; i < SLOTS; i++)
        slots[i] = *(volatile SceUInt32 *)sceKernelGetTLSAddr(i);
    info.size = sizeof(info);
    sceKernelGetThreadInfo(0, &info);
}

static int kind(SceUInt32 value, SceUID thread) {
    const SceUIntPtr low = (SceUIntPtr)info.stack, high = low + info.stackSize;
    if (value == (SceUInt32)thread)
        return THREAD_UID;
    if (value == low)
        return STACK_LOW;
    if (value == high)
        return STACK_HIGH;
    if (value > low && value < high)
        return ON_STACK;
    if (value == (SceUInt32)info.currentPriority)
        return PRIORITY;
    if (value == (SceUInt32)info.currentCpuAffinityMask)
        return AFFINITY;
    if (value == (SceUInt32)sceKernelGetProcessId())
        return PROCESS_ID;
    return OTHER;
}

static void print_slots(const char *name, SceUID thread) {
    int nonzero = 0;
    for (int i = 0; i < SLOTS; i++)
        nonzero += slots[i] != 0;
    PRINTF(nonzero, "%s: slots that aren't 0", name);
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i] != 0)
            PRINTF(kind(slots[i], thread), "%s: slot %d: what it holds", name, i);
    }
}

static int thread_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    read_slots();
    return 0;
}

int main(void) {
    PRINT("kinds: 1 other, 2 the thread's UID, 3 its stack's low end, 4 its high end, 5 on its stack, 6 its priority, 7 its affinity, 8 the process ID", 0);
    read_slots();
    print_slots("the main thread", sceKernelGetThreadId());
    const SceUID thread = sceKernelCreateThread("slots", thread_entry, 0x10000100, 0x4000, 0, 0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    print_slots("a new thread", thread);
    sceKernelDeleteThread(thread);
    test_exit();
    return 0;
}
