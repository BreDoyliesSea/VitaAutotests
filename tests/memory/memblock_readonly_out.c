// Whether the memory block calls write their results to memory the app can only read. The memory
// is a block opened with sceKernelOpenMemBlock with flags 0x10, which maps the memory of a 64 KiB
// USER_RW block with ENABLE_OPEN read only, so the test sets the words through the original and
// sees through it what a call wrote. Each case prints the result and whether the call changed
// the words it writes: sceKernelGetMemBlockBase, sceKernelGetMemBlockInfoByAddr and
// sceKernelGetMemBlockInfoByRange with the size of their struct set, and sceKernelGetFreeMemorySize
// with the size of its struct set. Then sceKernelGetMemBlockBase into a USER_MAIN_R block, and
// whether its first word changed and is the base. The app itself never writes to either read only
// mapping, since that faults on a Vita.
//
// On a PS Vita TV on firmware 3.65 every call given a pointer into the read only opened block, or
// into a USER_MAIN_R block, fails with 0x80022005 (INVALID_MEMORY_ACCESS) and writes nothing, so
// the kernel checks that the app may write there before it writes a result.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000
#define MARKER 0xA5A5A5A5
#define WORDS 16

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static volatile SceUInt32 *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

static volatile SceUInt32 *writable;
static void *readable;

// Sets the words through the writable mapping, the first to first and the rest to the marker
static void fill(SceUInt32 first) {
    writable[0] = first;
    for (int i = 1; i < WORDS; i++)
        writable[i] = MARKER;
}

// Whether any word changed from what fill set
static int changed(SceUInt32 first) {
    int any = writable[0] != first;
    for (int i = 1; i < WORDS; i++)
        any |= writable[i] != MARKER;
    return any;
}

int main(void) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID original = sceKernelAllocMemBlock("readonly out", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
        64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("allocate the original", test_created(original));
    const SceUID opened = original >= 0 ? sceKernelOpenMemBlock("readonly out", 0x10) : original;
    PRINT("open it read only", test_created(opened));
    if (opened < 0) {
        if (original >= 0)
            sceKernelFreeMemBlock(original);
        test_exit();
        return 0;
    }
    writable = base_of(original);
    readable = (void *)base_of(opened);
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    sceKernelGetMemBlockInfoByAddr(readable, &info);
    PRINT("the opened block's access", info.access);

    fill(MARKER);
    PRINT("get the base into it", sceKernelGetMemBlockBase(original, readable));
    PRINT("get the base into it: the words changed", changed(MARKER));

    fill(sizeof(SceKernelMemBlockInfo));
    PRINT("info by address into it", sceKernelGetMemBlockInfoByAddr((void *)writable, readable));
    PRINT("info by address into it: the words changed", changed(sizeof(SceKernelMemBlockInfo)));

    fill(sizeof(SceKernelMemBlockInfo));
    PRINT("info by range into it",
        sceKernelGetMemBlockInfoByRange((void *)writable, 4 * KIB, readable));
    PRINT("info by range into it: the words changed", changed(sizeof(SceKernelMemBlockInfo)));

    fill(sizeof(SceKernelFreeMemorySizeInfo));
    PRINT("free memory size into it", sceKernelGetFreeMemorySize(readable));
    PRINT("free memory size into it: the words changed",
        changed(sizeof(SceKernelFreeMemorySizeInfo)));

    sceKernelCloseMemBlock(opened);

    const SceUID read_only
        = sceKernelAllocMemBlock("readonly", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R, 4 * KIB, NULL);
    PRINT("USER_MAIN_R: allocate a block", test_created(read_only));
    if (read_only >= 0) {
        volatile SceUInt32 *words = base_of(read_only);
        const SceUInt32 word = words[0];
        PRINT("USER_MAIN_R: get the base into it",
            sceKernelGetMemBlockBase(original, (void **)words));
        PRINT("USER_MAIN_R: its first word changed", words[0] != word);
        PRINT("USER_MAIN_R: its first word is the base", words[0] == (SceUIntPtr)writable);
        sceKernelFreeMemBlock(read_only);
    }
    sceKernelFreeMemBlock(original);
    test_exit();
    return 0;
}
