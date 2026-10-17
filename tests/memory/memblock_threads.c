// How memory blocks live across threads. Each block is 64 KiB of USER_RW, holding a known word
// where a case reads it, and each thread has a stack of 0x4000. The main thread looks at what a
// thread left only once that thread ended. The cases:
// - a thread allocates a block, writes the word and ends: the main memory taken, measured to the
//   page the way app_free_sizes does, once it ended and once it's deleted, whether the block's UID
//   gives its base, whether the find by its base gives the UID and whether the word is there, then
//   the main thread's free and the memory taken after it
// - a thread that allocates a block and ends with sceKernelExitDeleteThread: the same lookups and
//   the free
// - a thread allocates a block with ENABLE_OPEN and opens it with flags 0x30, then ends: the find
//   by the opened block's base, whether the word is there, and the main thread's close of the
//   opened block and free of the original
// - the main thread allocates a block with ENABLE_OPEN and opens it, and a thread closes the opened
//   block and frees the original: their results, and the base by UID of both once it ended
// - a thread waits on a semaphore with a block's UID and base while the main thread frees the
//   block, and once woken looks up the base by the UID and the find by the base, and frees and
//   closes the UID
// - two threads start at once and allocate 16 blocks each: whether every block was made, has a UID
//   of its own and overlaps no other, and whether the main thread frees them all
// Each case prints its lines once it ends. A find prints 0 when it gives the block, the error when
// it fails and 1 when it gives another UID.
//
// On a PS Vita TV on firmware 3.65 a block outlives the thread that made it, whether that thread
// ends, is deleted, or exits and deletes itself: its UID and base stay, it holds its word, and the
// main thread frees it. The ended thread holds 88 KiB with its 64 KiB block, and 64 KiB once it is
// deleted, so its stack and TLS go with the delete. A block a thread opened outlives it too. A
// thread may close or free blocks another thread made, and a thread holding the UID of a block
// another thread freed gets 0x80024501 and 0x80022007. Two threads making blocks at once get blocks
// of their own that don't overlap.
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000
#define PROBES 320
#define MAX_NOTES 32
#define SIZE (64 * KIB)
#define WORD 0x7EADB10C
#define EACH 16

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static SceUID probes[PROBES];
static char note_labels[MAX_NOTES][112];
static int note_values[MAX_NOTES], note_count;

// What the threads share with the main thread
static volatile SceUID shared_block, shared_opened, start_sema;
static volatile SceUIntPtr shared_base;
static volatile int done, base_result, find_after, free_result, close_result;
static SceUID made_blocks[2][EACH];
static SceUIntPtr made_bases[2][EACH];

// Keeps a line of output, to print with the others once the case ends
__attribute__((format(printf, 2, 3))) static void note(int value, const char *fmt, ...) {
    if (note_count == MAX_NOTES)
        return;
    va_list args;
    va_start(args, fmt);
    sceClibVsnprintf(note_labels[note_count], sizeof(note_labels[0]), fmt, args);
    va_end(args);
    note_values[note_count++] = value;
}

static void print_notes(void) {
    for (int i = 0; i < note_count; i++)
        PRINT(note_labels[i], note_values[i]);
    note_count = 0;
}

static int reported(void) {
    SceKernelFreeMemorySizeInfo sizes = { .size = sizeof(sizes) };
    sceKernelGetFreeMemorySize(&sizes);
    return sizes.size_user;
}

// The main free size to the page, or -1 if it never drops
static int measure(void) {
    const int start = reported();
    int made = 0, now = start;
    while (made < PROBES && now == start) {
        const SceUID block
            = sceKernelAllocMemBlock("free probe", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, NULL);
        if (block < 0)
            break;
        probes[made++] = block;
        now = reported();
    }
    for (int i = 0; i < made; i++)
        sceKernelFreeMemBlock(probes[i]);
    return now == start ? -1 : start + (made - 1) * 4 * KIB;
}

// The KiB taken between two measures, or -1 if either failed
static int taken(int before, int now) {
    return before < 0 || now < 0 ? -1 : (before - now) / KIB;
}

static volatile SceUInt32 *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

static int find_result(SceUIntPtr base, SceUID block) {
    const SceUID found = sceKernelFindMemBlockByAddr((const void *)base, 0);
    return found == block ? 0 : found < 0 ? found : 1;
}

static SceUID start(SceKernelThreadEntry entry) {
    const SceUID thread = sceKernelCreateThread("memblock thread", entry,
                                                SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0,
                                                NULL);
    sceKernelStartThread(thread, 0, NULL);
    return thread;
}

// Allocates a block holding the word and shares it
static void alloc_shared(void) {
    const SceUID block
        = sceKernelAllocMemBlock("thread block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SIZE, NULL);
    shared_block = block;
    shared_base = block >= 0 ? (SceUIntPtr)base_of(block) : 0;
    if (shared_base)
        *(volatile SceUInt32 *)shared_base = WORD;
}

// Notes what the shared block's UID and base give, and whether it holds the word
static void note_lookups(const char *how) {
    void *out = NULL;
    note(sceKernelGetMemBlockBase(shared_block, &out), "%s: the block's base by its UID", how);
    note(out != NULL && (SceUIntPtr)out == shared_base, "%s: it's the base it had", how);
    const int found = shared_base ? find_result(shared_base, shared_block) : -1;
    note(found, "%s: find the block by its base", how);
    note(found == 0 && *(volatile SceUInt32 *)shared_base == WORD, "%s: the word is there", how);
}

static int alloc_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    alloc_shared();
    return 0;
}

static void ended_thread(void) {
    const char *how = "allocated by a thread that ended";
    const int before = measure();
    const SceUID thread = start(alloc_entry);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    const int ended = measure();
    const int deleted = sceKernelDeleteThread(thread);
    const int after_delete = measure();
    note(test_created(shared_block), "%s: the thread's allocation", how);
    note(taken(before, ended), "%s: KiB of main memory taken once it ended", how);
    note(deleted, "%s: delete the thread", how);
    note(taken(before, after_delete), "%s: KiB of main memory taken after that", how);
    note_lookups(how);
    note(sceKernelFreeMemBlock(shared_block), "%s: free it from the main thread", how);
    note(taken(before, measure()), "%s: KiB of main memory taken after that", how);
    print_notes();
}

static int exit_delete_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    alloc_shared();
    done = 1;
    sceKernelExitDeleteThread(0);
    return 0;
}

static void exit_deleted_thread(void) {
    const char *how = "allocated by a thread that exited and deleted itself";
    done = 0;
    const SceUID thread = start(exit_delete_entry);
    TEST_WAIT_FOR(done);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    TEST_WAIT_FOR(sceKernelGetThreadInfo(thread, &info) < 0);
    note(test_created(shared_block), "%s: the thread's allocation", how);
    note_lookups(how);
    note(sceKernelFreeMemBlock(shared_block), "%s: free it from the main thread", how);
    print_notes();
}

static int open_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    shared_block = sceKernelAllocMemBlock("thread open", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SIZE,
                                          (SceKernelAllocMemBlockOpt *)&opt);
    if (shared_block >= 0)
        base_of(shared_block)[0] = WORD;
    shared_opened = shared_block >= 0 ? sceKernelOpenMemBlock("thread open", 0x30) : shared_block;
    return 0;
}

static void opened_by_thread(void) {
    const char *how = "opened by a thread that ended";
    const SceUID thread = start(open_entry);
    test_join_thread(thread);
    note(test_created(shared_block), "%s: the thread's allocation", how);
    note(test_created(shared_opened), "%s: the thread's open", how);
    if (shared_opened >= 0) {
        volatile SceUInt32 *const theirs = base_of(shared_opened);
        const int found = theirs ? find_result((SceUIntPtr)theirs, shared_opened) : -1;
        note(found, "%s: find the opened block by its base", how);
        note(found == 0 && theirs[0] == WORD, "%s: the word is there", how);
        note(sceKernelCloseMemBlock(shared_opened), "%s: close the opened block", how);
    }
    if (shared_block >= 0)
        note(sceKernelFreeMemBlock(shared_block), "%s: free the original", how);
    print_notes();
}

static int close_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    close_result = sceKernelCloseMemBlock(shared_opened);
    free_result = sceKernelFreeMemBlock(shared_block);
    return 0;
}

static void ended_by_thread(void) {
    const char *how = "ended by another thread";
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    shared_block = sceKernelAllocMemBlock("thread close", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SIZE,
                                          (SceKernelAllocMemBlockOpt *)&opt);
    shared_opened = shared_block >= 0 ? sceKernelOpenMemBlock("thread close", 0x30) : shared_block;
    note(test_created(shared_block), "%s: allocate the original", how);
    note(test_created(shared_opened), "%s: open it", how);
    if (shared_opened < 0) {
        if (shared_block >= 0)
            sceKernelFreeMemBlock(shared_block);
        print_notes();
        return;
    }
    test_join_thread(start(close_entry));
    note(close_result, "%s: the thread's close of the opened block", how);
    note(free_result, "%s: the thread's free of the original", how);
    void *out = NULL;
    note(sceKernelGetMemBlockBase(shared_opened, &out), "%s: the opened block's base by its UID",
         how);
    note(sceKernelGetMemBlockBase(shared_block, &out), "%s: the original's base by its UID", how);
    print_notes();
}

static int holder_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(start_sema, 1, &timeout);
    void *out = NULL;
    base_result = sceKernelGetMemBlockBase(shared_block, &out);
    find_after = find_result(shared_base, shared_block);
    free_result = sceKernelFreeMemBlock(shared_block);
    close_result = sceKernelCloseMemBlock(shared_block);
    return 0;
}

static void freed_under_holder(void) {
    const char *how = "freed while another thread holds it";
    start_sema = sceKernelCreateSema("memblock holder", 0, 0, 1, NULL);
    alloc_shared();
    note(test_created(shared_block), "%s: allocate it", how);
    const SceUID thread = start(holder_entry);
    test_wait_until_waiting(thread);
    note(sceKernelFreeMemBlock(shared_block), "%s: free it", how);
    sceKernelSignalSema(start_sema, 1);
    test_join_thread(thread);
    note(base_result, "%s: the thread's base by the UID", how);
    note(find_after, "%s: the thread's find by the base", how);
    note(free_result, "%s: the thread's free", how);
    note(close_result, "%s: the thread's close", how);
    sceKernelDeleteSema(start_sema);
    print_notes();
}

static int many_entry(SceSize args, void *argp) {
    const int index = args >= sizeof(int) ? *(const int *)argp : 0;
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(start_sema, 1, &timeout);
    for (int i = 0; i < EACH; i++) {
        made_blocks[index][i] = sceKernelAllocMemBlock("thread many",
                                                       SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SIZE,
                                                       NULL);
        made_bases[index][i]
            = made_blocks[index][i] >= 0 ? (SceUIntPtr)base_of(made_blocks[index][i]) : 0;
    }
    return 0;
}

static void two_at_once(void) {
    const char *how = "two threads at once";
    start_sema = sceKernelCreateSema("memblock start", 0, 0, 2, NULL);
    const SceUID first = test_start_thread_arg(many_entry, 0);
    const SceUID second = test_start_thread_arg(many_entry, 1);
    test_wait_until_waiting(first);
    test_wait_until_waiting(second);
    sceKernelSignalSema(start_sema, 2);
    test_join_thread(first);
    test_join_thread(second);
    sceKernelDeleteSema(start_sema);

    const SceUID *uids = &made_blocks[0][0];
    const SceUIntPtr *bases = &made_bases[0][0];
    int all_made = 1, own_uids = 1, apart = 1, freed = 0;
    for (int i = 0; i < 2 * EACH; i++) {
        all_made &= uids[i] >= 0 && bases[i] != 0;
        for (int j = i + 1; j < 2 * EACH; j++) {
            own_uids &= uids[i] != uids[j];
            apart &= bases[i] + SIZE <= bases[j] || bases[j] + SIZE <= bases[i];
        }
    }
    for (int i = 0; i < 2 * EACH; i++)
        freed += uids[i] >= 0 && sceKernelFreeMemBlock(uids[i]) == 0;
    note(all_made, "%s: every block is made", how);
    note(own_uids, "%s: each has a UID of its own", how);
    note(apart, "%s: no two overlap", how);
    note(freed == 2 * EACH, "%s: the main thread frees them all", how);
    print_notes();
}

int main(void) {
    // The first measure only warms up
    measure();
    ended_thread();
    exit_deleted_thread();
    opened_by_thread();
    ended_by_thread();
    freed_under_holder();
    two_at_once();
    test_exit();
    return 0;
}
