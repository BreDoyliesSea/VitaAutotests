// Freeing a block that only a USSE map holds, and one whose sceGxmMapMemory mapping sceGxmTerminate
// dropped. No GPU work runs. Each case has a fresh 64 KiB block of USER_RW, and after each step
// notes what its UID and its base give to sceKernelGetMemBlockBase, sceKernelFindMemBlockByAddr and
// sceKernelGetMemBlockInfoByAddr, and the main memory it takes, measured to the page the way
// app_free_sizes does. The cases:
// - the whole block mapped with sceGxmMapMemory, then sceGxmTerminate and sceGxmInitialize again,
//   then the free
// - 4 KiB of it mapped with sceGxmMapVertexUsseMemory: the free, then the USSE unmap, then a
//   second free
// - the same with sceGxmMapFragmentUsseMemory
// The USSE cases come last, since a wrong implementation that frees a block a USSE map holds may
// crash on the unmap. Nothing reads or writes the blocks, and each case prints its lines once it
// ends.
//
// A find prints 0 when it gives the block, the error when it fails and 1 when it gives another
// UID.
//
// On a PS Vita TV on firmware 3.65 sceGxmTerminate drops the mapping: after it and a new initialize
// the block frees, its memory comes back and its address is in no block (0x80022007). A vertex or
// fragment USSE mapping keeps its block as sceGxmMapMemory does: the free gives 0x80024703
// (BLOCK_IN_USE) and drops the UID, the info call still reports the block, its 64 KiB stay taken,
// also after the unmap, and freeing it again gives 0x80024501.
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define PROBES 320
#define MAX_NOTES 48
#define SIZE (64 * KIB)

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

typedef struct Block {
    const char *label;
    SceUID uid;
    char *base;
    // The main free size before it was made
    int start;
} Block;

static SceUID probes[PROBES];
static char note_labels[MAX_NOTES][112];
static int note_values[MAX_NOTES], note_count;

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

static void display_callback(const void *data) {
    (void)data;
}

static int initialize(void) {
    SceGxmInitializeParams init = { 0 };
    init.displayQueueMaxPendingCount = 2;
    init.displayQueueCallback = display_callback;
    init.displayQueueCallbackDataSize = 4;
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    return sceGxmInitialize(&init);
}

static int make(Block *block, const char *label) {
    block->label = label;
    block->start = measure();
    block->uid
        = sceKernelAllocMemBlock("gxm usse free", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SIZE, NULL);
    note(test_created(block->uid), "%s: allocate it", label);
    if (block->uid < 0) {
        print_notes();
        return 0;
    }
    void *base = NULL;
    sceKernelGetMemBlockBase(block->uid, &base);
    block->base = base;
    return 1;
}

// Notes what the block's UID and base give, with nothing made since the step, then the KiB of
// main memory it takes
static void look(const Block *block, const char *when) {
    void *out = NULL;
    note(sceKernelGetMemBlockBase(block->uid, &out), "%s, %s: its base by its UID", block->label,
         when);
    const SceUID found = sceKernelFindMemBlockByAddr(block->base, 0);
    note(found == block->uid ? 0 : found < 0 ? found : 1, "%s, %s: find by its base",
         block->label, when);
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    note(sceKernelGetMemBlockInfoByAddr(block->base, &info), "%s, %s: info by its base",
         block->label, when);
    const int now = measure();
    note(now < 0 || block->start < 0 ? -1 : (block->start - now) / KIB,
         "%s, %s: KiB of main memory it takes", block->label, when);
}

static void terminated(void) {
    Block block;
    const char *label = "mapped at the terminate";
    if (!make(&block, label))
        return;
    note(sceGxmMapMemory(block.base, SIZE, RW), "%s: map it", label);
    note(sceGxmTerminate(), "%s: terminate libgxm", label);
    note(initialize(), "%s: initialize libgxm again", label);
    look(&block, "before the free");
    note(sceKernelFreeMemBlock(block.uid), "%s: free it", label);
    look(&block, "after the free");
    print_notes();
}

static void usse(const char *label, int fragment) {
    Block block;
    if (!make(&block, label))
        return;
    SceUInt32 offset = 0;
    const int mapped = fragment ? sceGxmMapFragmentUsseMemory(block.base, 4 * KIB, &offset)
                                : sceGxmMapVertexUsseMemory(block.base, 4 * KIB, &offset);
    note(mapped, "%s: map 4 KiB of it", label);
    look(&block, "while it's mapped");
    note(sceKernelFreeMemBlock(block.uid), "%s: free it", label);
    look(&block, "after the free");
    if (mapped == 0) {
        note(fragment ? sceGxmUnmapFragmentUsseMemory(block.base)
                      : sceGxmUnmapVertexUsseMemory(block.base),
             "%s: unmap it", label);
        look(&block, "after the unmap");
    }
    note(sceKernelFreeMemBlock(block.uid), "%s: free it again", label);
    print_notes();
}

int main(void) {
    PRINT("initialize libgxm", initialize());
    // The first measure only warms up
    measure();
    terminated();
    usse("vertex USSE", 0);
    usse("fragment USSE", 1);
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
