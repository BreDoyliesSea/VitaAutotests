// More rules for the names of openable sync objects, past name_lookup and open. The cases:
// - an openable semaphore with a 31 character name, opened by that name and by that name with one
//   more character
// - an openable semaphore made before one with its name that isn't openable, which one an open of
//   the name gives, and an open once the openable one is deleted
// - two openable semaphores with the empty name
// - an openable semaphore with the name of a thread that never started, of a callback, of a
//   lightweight mutex and of a memory block with ENABLE_OPEN, each made first, and a memory block
//   with ENABLE_OPEN with the name of an openable semaphore made first. For each, whether the
//   semaphore is made and which one an open gives, and for the memory blocks whether the block is
//   made and whether sceKernelOpenMemBlock still gives it.
// A semaphore an open gives prints 1 for the first one made and 2 for the second, told apart by
// their counts, and 0 for neither. An opened block prints 1 when it reads the word the block
// holds and 0 when it doesn't.
//
// On a PS Vita TV on firmware 3.65 a 31 character name opens, and one more character fails with
// 0x80024002 (UID_NAME_TOO_LONG). A semaphore that isn't openable takes the name of an openable
// one, an open still gives the openable one, and once that is deleted the name no longer opens,
// with 0x8002450A (UID_CANNOT_FIND_BY_NAME), though the other semaphore still has it. A second
// openable semaphore with the empty name fails with 0x80024E02 (DUPLICATE_NAME), and an open of it
// gives the first. A thread, a callback and a lightweight mutex leave their names free for an
// openable semaphore. A memory block with ENABLE_OPEN and an openable semaphore can't share a name
// in either order, the second fails with DUPLICATE_NAME, and sceKernelOpenSema on the block's name
// gives 0x80028002 (DIFFERENT_UID_CLASS) and sceKernelOpenMemBlock on the semaphore's 0x80024508
// (INVALID_UID_CLASS).
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NAME_31 "abcdefghijklmnopqrstuvwxyz01234"
#define NAME_32 NAME_31 "5"

#define KIB 0x400
#define ENABLE_OPEN 0x4000
#define BLOCK_WORD 0x11111111

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static int callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    return 0;
}

static int idle(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

// An openable semaphore holding number as its count
static SceUID make_sema(const char *name, SceUInt32 attr, int number) {
    return sceKernelCreateSema(name, attr, number, 4, NULL);
}

// Which semaphore an open of name gives, or the open's error
static int open_which(const char *name) {
    const SceUID opened = sceKernelOpenSema(name);
    if (opened < 0)
        return opened;
    SceKernelSemaInfo info = { .size = sizeof(info) };
    int which = 0;
    const int count = sceKernelGetSemaInfo(opened, &info) == 0 ? info.currentCount : 0;
    if (count == 1 || count == 2)
        which = count;
    sceKernelCloseSema(opened);
    return which;
}

// A 64 KiB USER_RW block with ENABLE_OPEN holding BLOCK_WORD
static SceUID make_block(const char *name) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID block = sceKernelAllocMemBlock(
        name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    void *base = NULL;
    if (block >= 0 && sceKernelGetMemBlockBase(block, &base) == 0 && base)
        *(volatile SceUInt32 *)base = BLOCK_WORD;
    return block;
}

// Whether an open of name with flags 0x10 gives a block that reads BLOCK_WORD, or the open's error
static int open_block(const char *name) {
    const SceUID opened = sceKernelOpenMemBlock(name, 0x10);
    if (opened < 0)
        return opened;
    void *base = NULL;
    int reads = 0;
    // Read only while the block is found by its base, since a stale mapping faults
    if (sceKernelGetMemBlockBase(opened, &base) == 0 && base
        && sceKernelFindMemBlockByAddr(base, 0) == opened)
        reads = *(volatile SceUInt32 *)base == BLOCK_WORD;
    sceKernelCloseMemBlock(opened);
    return reads;
}

// Makes an openable semaphore with the name of something made first, and opens the name
static void after(const char *label, const char *name, SceUID first) {
    PRINTF(test_created(first), "%s: make it", label);
    const SceUID sema = make_sema(name, SCE_KERNEL_ATTR_OPENABLE, 1);
    PRINTF(test_created(sema), "%s: create an openable semaphore with its name", label);
    PRINTF(open_which(name), "%s: the semaphore an open gives", label);
    if (sema >= 0)
        sceKernelDeleteSema(sema);
}

int main(void) {
    SceUID first = make_sema(NAME_31, SCE_KERNEL_ATTR_OPENABLE, 1);
    PRINT("a 31 character name: create", test_created(first));
    PRINT("a 31 character name: open by it", open_which(NAME_31));
    PRINT("a 31 character name: open by it and one more character", open_which(NAME_32));
    sceKernelDeleteSema(first);

    first = make_sema("Order", SCE_KERNEL_ATTR_OPENABLE, 1);
    SceUID second = make_sema("Order", 0, 2);
    PRINT("openable first: create the one that isn't openable", test_created(second));
    PRINT("openable first: the semaphore an open gives", open_which("Order"));
    sceKernelDeleteSema(first);
    PRINT("openable first: an open once the openable one is deleted", open_which("Order"));
    sceKernelDeleteSema(second);

    first = make_sema("", SCE_KERNEL_ATTR_OPENABLE, 1);
    second = make_sema("", SCE_KERNEL_ATTR_OPENABLE, 2);
    PRINT("the empty name: create a second openable semaphore with it", test_created(second));
    PRINT("the empty name: the semaphore an open gives", open_which(""));
    sceKernelDeleteSema(first);
    if (second >= 0)
        sceKernelDeleteSema(second);

    const SceUID thread = sceKernelCreateThread(
        "Thread name", idle, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    after("a thread", "Thread name", thread);
    sceKernelDeleteThread(thread);

    const SceUID cb = sceKernelCreateCallback("Callback name", 0, callback, NULL);
    after("a callback", "Callback name", cb);
    sceKernelDeleteCallback(cb);

    static SceKernelLwMutexWork lw_mutex;
    const int lw_result = sceKernelCreateLwMutex(&lw_mutex, "Lw mutex name", 0, 0, NULL);
    after("a lightweight mutex", "Lw mutex name", lw_result);
    if (lw_result >= 0)
        sceKernelDeleteLwMutex(&lw_mutex);

    const SceUID block = make_block("Block name");
    after("a memory block", "Block name", block);
    PRINT("a memory block: an open of the block after", open_block("Block name"));
    if (block >= 0)
        sceKernelFreeMemBlock(block);

    const SceUID sema = make_sema("Sema name", SCE_KERNEL_ATTR_OPENABLE, 1);
    PRINT("a block after a semaphore: create the semaphore", test_created(sema));
    const SceUID late = make_block("Sema name");
    PRINT("a block after a semaphore: make a block with its name", test_created(late));
    PRINT("a block after a semaphore: an open of the block", open_block("Sema name"));
    PRINT("a block after a semaphore: the semaphore an open gives", open_which("Sema name"));
    if (late >= 0)
        sceKernelFreeMemBlock(late);
    sceKernelDeleteSema(sema);

    test_exit();
    return 0;
}
