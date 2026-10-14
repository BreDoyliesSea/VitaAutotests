// The fields of a simple event's info: the attribute as given and as reported, the pattern and
// user data after a set, a clear and a pulse, the waiting thread count with 0 and 2 waiters, the
// name for 31, 32 and 40 character names, and info sizes of 0, below the struct and past it. Kept
// short and on its own, so a hang in it loses nothing else.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NAME_31 "abcdefghijklmnopqrstuvwxyz01234"
#define NAME_32 NAME_31 "5"
#define NAME_40 NAME_32 "6789ABCD"

static SceUID event;

static int wait_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = TEST_WAIT_US, result;
    SceUInt64 user_data;
    sceKernelWaitEvent(event, EVENT_BIT_D, &result, &user_data, &timeout);
    return 0;
}

static EventInfo info_of(SceUID id) {
    EventInfo info = { .size = sizeof(info) };
    sceKernelGetEventInfo(id, &info);
    return info;
}

// The user data is left out after create, where it holds leftover data that differs between runs
static void print_info(const char *when, SceUID id, int user_data) {
    const EventInfo info = info_of(id);
    PRINTF(info.eventPattern, "%s: the pattern", when);
    if (!user_data)
        return;
    PRINTF((SceUInt32)info.userData, "%s: the low word of the user data", when);
    PRINTF((SceUInt32)(info.userData >> 32), "%s: the high word of the user data", when);
}

int main(void) {
    event = sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET | SCE_KERNEL_ATTR_THREAD_PRIO, EVENT_BIT_A, NULL);
    EventInfo info = { .size = sizeof(info) };
    PRINT("info", sceKernelGetEventInfo(event, &info));
    PRINT("the attribute", info.attr);
    PRINT("the name", sceClibStrcmp(info.name, "event") == 0);
    PRINT("the waiting threads", info.numWaitThreads);
    PRINT("the reserved word", info.reserved);
    print_info("after create", event, 0);
    sceKernelSetEvent(event, EVENT_BIT_B, 0x123456789ABCDEF0ull);
    print_info("after a set", event, 1);
    sceKernelClearEvent(event, EVENT_BIT_A);
    print_info("after a clear", event, 1);
    sceKernelPulseEvent(event, EVENT_BIT_C, 0x42);
    print_info("after a pulse with no waiter", event, 1);
    sceKernelDeleteSimpleEvent(event);

    // Waiters on a manual reset event, so one set wakes both
    event = sceKernelCreateSimpleEvent("waited", EVENT_ATTR_MANUAL_RESET, 0, NULL);
    SceUID threads[2];
    for (int i = 0; i < 2; i++) {
        threads[i] = test_start_thread(wait_thread);
        test_wait_until_waiting(threads[i]);
    }
    PRINT("the waiting threads with 2 waiters", info_of(event).numWaitThreads);
    sceKernelSetEvent(event, EVENT_BIT_D, 0);
    for (int i = 0; i < 2; i++)
        test_join_thread(threads[i]);
    PRINT("the waiting threads after they woke", info_of(event).numWaitThreads);

    // Names
    static const char *const names[] = { NAME_31, NAME_32, NAME_40 };
    static const int lengths[] = { 31, 32, 40 };
    for (int i = 0; i < 3; i++) {
        const SceUID named = sceKernelCreateSimpleEvent(names[i], 0, 0, NULL);
        if (named < 0) {
            PRINTF(named, "create with a %d character name", lengths[i]);
            continue;
        }
        const EventInfo named_info = info_of(named);
        int len = 0;
        while (len < 32 && named_info.name[len] != '\0')
            len++;
        PRINTF(len, "the reported length of a %d character name", lengths[i]);
        PRINTF(sceClibMemcmp(named_info.name, names[i], len) == 0, "it is the start of the %d character name", lengths[i]);
        sceKernelDeleteSimpleEvent(named);
    }

    // Sizes, each run twice into a buffer filled with another byte, to see how far it writes
    static union {
        SceUInt64 align;
        unsigned char bytes[sizeof(EventInfo) + 64];
    } buf[2];
    const SceSize sizes[] = { 0, 4, 8, 40, sizeof(EventInfo) - 4, sizeof(EventInfo) + 4 };
    for (unsigned int s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int result = 0;
        for (int pass = 0; pass < 2; pass++) {
            sceClibMemset(buf[pass].bytes, pass ? 0x11 : 0xEE, sizeof(buf[pass].bytes));
            *(SceSize *)buf[pass].bytes = sizes[s];
            result = sceKernelGetEventInfo(event, (EventInfo *)buf[pass].bytes);
        }
        int written = 0;
        for (int i = sizeof(SceSize); i < (int)sizeof(buf[0].bytes); i++) {
            if (buf[0].bytes[i] != 0xEE || buf[1].bytes[i] != 0x11)
                written = i + 1;
        }
        PRINTF(result, "info with a size of %d", (int)sizes[s]);
        PRINTF(written, "the end of what it writes with a size of %d", (int)sizes[s]);
    }

    sceKernelDeleteSimpleEvent(event);
    test_exit();
    return 0;
}
