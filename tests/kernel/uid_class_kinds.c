// The class sceKernelGetThreadmgrUIDClass reports for UIDs past uid_class, and what UIDs a thread
// made are worth once it is gone. The cases:
// - a thread before its start, while it waits, after it ended, after it was deleted, and one that
//   ended with sceKernelExitDeleteThread, and whether a new thread gets the UID of a deleted one
// - a memory block, one opened from it with sceKernelOpenMemBlock, and each once it is closed
// - a callback once it is deleted, and one whose thread ended
// - a semaphore that isn't openable and an openable one, made by a thread, and a UID the thread
//   opened from the openable one, after the thread ended and was deleted: their class, and
//   whether a signal and a poll through each work
// A class prints as the number a Vita gives, or as the error.
//
// On a PS Vita TV on firmware 3.65 a thread has class 1 before its start, while it waits and after
// it ended, and once it is deleted, by sceKernelDeleteThread or sceKernelExitDeleteThread, its
// class call gives 0x80024501 (INVALID_UID). A new thread doesn't get the deleted thread's UID. For
// a memory block and one opened from it the class call gives 0x80020005 (INVALID_ARGUMENT), and
// INVALID_UID once they are closed or freed. A callback is gone once it is deleted, and also once
// the thread that made it ends and is deleted. The semaphores a thread made, openable or not, and a
// UID it opened outlive the thread, with class 2, and a signal and a poll through each work.
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static SceUID gate, made_cb, made_plain, made_openable, made_opened;

static int callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    return 0;
}

// Waits on the gate semaphore, then ends
static int gated(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(gate, 1, &timeout);
    return 0;
}

static int exit_delete(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelExitDeleteThread(0);
    return 0;
}

// Makes a callback, then ends
static int callback_maker(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    made_cb = sceKernelCreateCallback("made", 0, callback, NULL);
    return 0;
}

// Makes two semaphores and opens the openable one, then ends
static int object_maker(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    made_plain = sceKernelCreateSema("made plain", 0, 0, 4, NULL);
    made_openable = sceKernelCreateSema("made openable", SCE_KERNEL_ATTR_OPENABLE, 0, 4, NULL);
    made_opened = sceKernelOpenSema("made openable");
    return 0;
}

static SceUID create_thread(SceKernelThreadEntry entry) {
    return sceKernelCreateThread(
        "class", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
}

static void threads(void) {
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);
    const SceUID thread = create_thread(gated);
    PRINT("a thread before its start", get_uid_class(thread));
    sceKernelStartThread(thread, 0, NULL);
    test_wait_until_waiting(thread);
    PRINT("a thread while it waits", get_uid_class(thread));
    sceKernelSignalSema(gate, 1);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    PRINT("a thread after it ended", get_uid_class(thread));
    sceKernelDeleteThread(thread);
    PRINT("a thread after it was deleted", get_uid_class(thread));
    const SceUID next = create_thread(gated);
    PRINT("a new thread gets the deleted thread's UID", next == thread);
    sceKernelDeleteThread(next);
    sceKernelDeleteSema(gate);

    const SceUID self_deleting = create_thread(exit_delete);
    sceKernelStartThread(self_deleting, 0, NULL);
    sceKernelWaitThreadEnd(self_deleting, NULL, NULL);
    // A thread that deleted itself may be gone before the wait for its end starts
    TEST_WAIT_FOR(get_uid_class(self_deleting) < 0);
    PRINT("a thread that ended with sceKernelExitDeleteThread", get_uid_class(self_deleting));
}

static void memory_blocks(void) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID block = sceKernelAllocMemBlock("class block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("a memory block: allocate it", test_created(block));
    PRINT("a memory block", get_uid_class(block));
    const SceUID opened = sceKernelOpenMemBlock("class block", 0x10);
    PRINT("an opened memory block: open it", test_created(opened));
    PRINT("an opened memory block", get_uid_class(opened));
    sceKernelCloseMemBlock(opened);
    PRINT("an opened memory block after it is closed", get_uid_class(opened));
    sceKernelFreeMemBlock(block);
    PRINT("a memory block after it is freed", get_uid_class(block));
}

static void callbacks(void) {
    const SceUID cb = sceKernelCreateCallback("class", 0, callback, NULL);
    sceKernelDeleteCallback(cb);
    PRINT("a callback after it is deleted", get_uid_class(cb));

    const SceUID thread = create_thread(callback_maker);
    sceKernelStartThread(thread, 0, NULL);
    test_join_thread(thread);
    PRINT("a callback whose thread ended: it was made", test_created(made_cb));
    PRINT("a callback whose thread ended", get_uid_class(made_cb));
}

static void made_by_thread(void) {
    const SceUID thread = create_thread(object_maker);
    sceKernelStartThread(thread, 0, NULL);
    test_join_thread(thread);
    const SceUID uids[] = { made_plain, made_openable, made_opened };
    static const char *const names[] = {
        "a semaphore a deleted thread made",
        "an openable semaphore a deleted thread made",
        "a UID a deleted thread opened",
    };
    for (int i = 0; i < 3; i++) {
        PRINTF(test_created(uids[i]), "%s: it was made", names[i]);
        PRINTF(get_uid_class(uids[i]), "%s: its class", names[i]);
        PRINTF(sceKernelSignalSema(uids[i], 1), "%s: signal through it", names[i]);
        PRINTF(sceKernelPollSema(uids[i], 1), "%s: poll through it", names[i]);
    }
    sceKernelCloseSema(made_opened);
    sceKernelDeleteSema(made_openable);
    sceKernelDeleteSema(made_plain);
}

int main(void) {
    threads();
    memory_blocks();
    callbacks();
    made_by_thread();
    test_exit();
    return 0;
}
