// Waits that block, and waits for a thread that ended, given a timeout or an out value in memory
// the app can only read, as readonly_timeouts and readonly_out_values do for waits that end at
// once. The memory is a block opened with sceKernelOpenMemBlock with flags 0x10, which maps the
// memory of a 64 KiB USER_RW block with ENABLE_OPEN read only, so the test sets the value through
// the original and sees through it whether a call wrote it. Each case, on fresh objects, prints
// the result and whether the call changed the words of the view. The cases:
// - sceKernelWaitThreadEnd on an ended thread with the status in the view, once with a timeout in
//   writable memory and once with no timeout, then once with the status in writable memory
// - a message pipe receive of 4 bytes on an empty pipe with a timeout of 10 ms in the view
// - the same receive with a timeout of 2 s in the view, on a thread of its own, woken by a send of
//   4 bytes, which also prints whether the receiver got the message
// - an event flag wait with the out bits in the view and a timeout of 1 s in writable memory, on a
//   thread of its own, woken by a set of the flag after the thread waits, which also prints
//   whether the timeout still holds 1 s
// A waiter thread that hasn't ended after 1 s is freed by deleting its object, and the test prints
// whether it ended by itself. The app itself never writes to the read only mapping, since that
// faults on a Vita.
//
// On a PS Vita TV on firmware 3.65 the wait for the end of an ended thread returns 0 when it is
// given a timeout and 0x80022005 (INVALID_MEMORY_ACCESS) when it isn't, and writes nothing to the
// status in both. With the status in writable memory it returns 0 and the exit status 0x1234. The
// receive that times out returns 0x80028005 (TIMEOUT) and leaves the view as it was. The receive
// woken by the send returns 0 with the message, and leaves the view as it was too. The event flag
// wait woken by the set returns 0x80022005, since it can't write the bits, writes nothing to the
// view and doesn't copy its time left back, so the timeout still holds 1 s.
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000
#define MARKER 0xA5A5A5A5
#define WORDS 16
#define GIVEN 1000000
#define SHORT 10000
#define LONG 2000000
#define STATUS 0x1234

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static volatile SceUInt32 *writable;
static void *readable;
static SceUID object;
static volatile int wait_result;
static volatile SceUInt writable_timeout;
static char received[4];
static const char message[4] = { 1, 2, 3, 4 };

static volatile SceUInt32 *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

// Sets the first word through the writable mapping to value, and the words after it to the marker
static void fill(SceUInt32 value) {
    writable[0] = value;
    for (int i = 1; i < WORDS; i++)
        writable[i] = MARKER;
}

// Whether any word changed from what fill set
static int changed(SceUInt32 value) {
    int any = writable[0] != value;
    for (int i = 1; i < WORDS; i++)
        any |= writable[i] != MARKER;
    return any;
}

static void print_wait(const char *name, int result, SceUInt32 value) {
    PRINTF(result, "%s", name);
    PRINTF(changed(value), "%s: the words changed", name);
}

static int ends_with_status(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return STATUS;
}

static int receiver(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    wait_result = sceKernelReceiveMsgPipe(
        object, received, sizeof(received), MSG_PIPE_MODE_FULL, NULL, readable);
    return 0;
}

static int flag_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    wait_result = sceKernelWaitEventFlag(
        object, 0x1, SCE_EVENT_WAITOR, readable, (SceUInt *)&writable_timeout);
    return 0;
}

// Waits 1 s for the thread to end, deleting the object with end_object if it hasn't, then deletes
// the thread and returns whether it ended by itself
static int join_or_delete(SceUID thread, int (*end_object)(SceUID)) {
    SceUInt join = 1000000;
    const int by_itself = sceKernelWaitThreadEnd(thread, NULL, &join) == 0;
    if (!by_itself) {
        end_object(object);
        object = -1;
        sceKernelWaitThreadEnd(thread, NULL, NULL);
    }
    sceKernelDeleteThread(thread);
    return by_itself;
}

static void thread_end_cases(void) {
    const SceUID thread = test_start_thread(ends_with_status);
    sceKernelWaitThreadEnd(thread, NULL, NULL);

    SceUInt timeout = GIVEN;
    fill(MARKER);
    print_wait("end wait with the status there and a timeout",
        sceKernelWaitThreadEnd(thread, readable, &timeout), MARKER);
    fill(MARKER);
    print_wait("end wait with the status there and no timeout",
        sceKernelWaitThreadEnd(thread, readable, NULL), MARKER);
    int status = 0xDEAD;
    timeout = GIVEN;
    PRINT("end wait with the status in writable memory",
        sceKernelWaitThreadEnd(thread, &status, &timeout));
    PRINT("end wait with the status in writable memory: the status", status);
    sceKernelDeleteThread(thread);
}

static void pipe_cases(void) {
    object = sceKernelCreateMsgPipe("ro waits", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    fill(SHORT);
    print_wait("receive that times out with the timeout there",
        sceKernelReceiveMsgPipe(
            object, received, sizeof(received), MSG_PIPE_MODE_FULL, NULL, readable),
        SHORT);
    sceKernelDeleteMsgPipe(object);

    const char *name = "receive woken by a send with the timeout there";
    object = sceKernelCreateMsgPipe("ro waits", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    sceClibMemset(received, 0, sizeof(received));
    wait_result = 0xDEAD;
    fill(LONG);
    const SceUID thread = test_start_thread(receiver);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_MSG_PIPE, object));
    const int sent = sceKernelSendMsgPipe(
        object, (void *)message, sizeof(message), MSG_PIPE_MODE_FULL, NULL, NULL);
    const int by_itself = join_or_delete(thread, sceKernelDeleteMsgPipe);
    PRINTF(sent, "%s: the send", name);
    PRINTF(by_itself, "%s: it ended by itself", name);
    print_wait(name, wait_result, LONG);
    PRINTF(sceClibMemcmp(received, message, sizeof(message)) == 0, "%s: it got the message", name);
    if (object >= 0)
        sceKernelDeleteMsgPipe(object);
}

static void event_flag_cases(void) {
    const char *name = "event flag wait woken by a set with the bits there";
    object = sceKernelCreateEventFlag("ro waits", 0, 0, NULL);
    writable_timeout = GIVEN;
    wait_result = 0xDEAD;
    fill(MARKER);
    const SceUID thread = test_start_thread(flag_waiter);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_EVENT_FLAG, object));
    const int set = sceKernelSetEventFlag(object, 0x1);
    const int by_itself = join_or_delete(thread, sceKernelDeleteEventFlag);
    PRINTF(set, "%s: the set", name);
    PRINTF(by_itself, "%s: it ended by itself", name);
    print_wait(name, wait_result, MARKER);
    PRINTF(writable_timeout == GIVEN, "%s: the timeout still holds its value", name);
    if (object >= 0)
        sceKernelDeleteEventFlag(object);
}

int main(void) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID original = sceKernelAllocMemBlock("ro waits", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("allocate the original", test_created(original));
    const SceUID opened = original >= 0 ? sceKernelOpenMemBlock("ro waits", 0x10) : original;
    PRINT("open it read only", test_created(opened));
    if (opened < 0) {
        if (original >= 0)
            sceKernelFreeMemBlock(original);
        test_exit();
        return 0;
    }
    writable = base_of(original);
    readable = (void *)base_of(opened);

    thread_end_cases();
    pipe_cases();
    event_flag_cases();

    sceKernelCloseMemBlock(opened);
    sceKernelFreeMemBlock(original);
    test_exit();
    return 0;
}
