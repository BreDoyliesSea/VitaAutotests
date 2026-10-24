// What sceKernelWaitMultipleEvents leaves in the entries of the result array past the ones it
// returns. The array has 4 entries filled with 0xAA bytes before each call, and every case prints
// the call's result and, for entries 1 to 3, whether the entry still holds only 0xAA bytes. What
// an overwritten entry holds instead isn't printed, since it is whatever the system's buffer held
// before. The events are simple events and no call waits. Cases:
// - OR on four events where only the third is set before the call
// - AND on four events that are all set before the call, so the entries are all filled
// - OR on four entries where the first is an unknown UID, and the other three events are not set
//
// On a PS Vita TV on firmware 3.65 the whole array is written back on every call. The OR wait on
// the four events returns 1 and fills entry 0 with the third event, and entries 1 to 3 no longer
// hold the 0xAA bytes. The AND wait returns 4 and fills every entry. The wait with an unknown UID
// first returns 1 with the entry 0x80028201, and entries 1 to 3 are overwritten again.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define EVENT_COUNT 4
#define UNKNOWN_UID 0x12345

static SceUID events[EVENT_COUNT];
static WaitEvent list[EVENT_COUNT];
static ResultEvent results[EVENT_COUNT];

// Creates four events that wait for EVENT_BIT_A, and fills the results with 0xAA bytes
static void setup(void) {
    for (int i = 0; i < EVENT_COUNT; i++) {
        events[i] = sceKernelCreateSimpleEvent("event", EVENT_ATTR_MANUAL_RESET, 0, NULL);
        list[i] = (WaitEvent){ events[i], EVENT_BIT_A };
    }
    sceClibMemset(results, 0xAA, sizeof(results));
}

static void teardown(void) {
    for (int i = 0; i < EVENT_COUNT; i++)
        sceKernelDeleteSimpleEvent(events[i]);
}

// Returns whether every byte of the result entry is value
static int entry_is(const ResultEvent *entry, unsigned char value) {
    const unsigned char *bytes = (const unsigned char *)entry;
    for (unsigned int i = 0; i < sizeof(*entry); i++) {
        if (bytes[i] != value)
            return 0;
    }
    return 1;
}

// Prints whether entries 1 to 3 still hold the 0xAA bytes
static void print_past(const char *name) {
    for (int i = 1; i < EVENT_COUNT; i++)
        PRINTF(entry_is(&results[i], 0xAA), "%s: entry %d is all 0xAA", name, i);
}

int main(void) {
    SceUInt32 timeout;

    // OR with only the third event set
    const char *name = "OR, only the third set";
    setup();
    sceKernelSetEvent(events[2], EVENT_BIT_A, 0x77);
    timeout = 1000;
    PRINTF(sceKernelWaitMultipleEvents(list, EVENT_COUNT, EVENT_WAIT_MODE_OR, results, &timeout), "%s: the call", name);
    PRINTF(results[0].eventId == events[2], "%s: entry 0 is for the third event", name);
    PRINTF(results[0].result, "%s: entry 0 result", name);
    print_past(name);
    teardown();

    // AND with all four set, so every entry is filled
    name = "AND, all four set";
    setup();
    for (int i = 0; i < EVENT_COUNT; i++)
        sceKernelSetEvent(events[i], EVENT_BIT_A, 0x77);
    timeout = 1000;
    PRINTF(sceKernelWaitMultipleEvents(list, EVENT_COUNT, EVENT_WAIT_MODE_AND, results, &timeout), "%s: the call", name);
    print_past(name);
    teardown();

    // OR with an unknown UID first, which finishes the call at once with one entry
    name = "OR, an unknown UID first";
    setup();
    list[0].eventId = UNKNOWN_UID;
    timeout = 1000;
    PRINTF(sceKernelWaitMultipleEvents(list, EVENT_COUNT, EVENT_WAIT_MODE_OR, results, &timeout), "%s: the call", name);
    PRINTF(results[0].result, "%s: entry 0 result", name);
    print_past(name);
    teardown();

    test_exit();
    return 0;
}
