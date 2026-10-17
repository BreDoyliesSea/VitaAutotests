// How the memory of a block opened with sceKernelOpenMemBlock and of its original lives on when one
// of them ends while sceGxmMapMemory has one or both mapped. Each original is 64 KiB of USER_RW
// with ENABLE_OPEN, opened with flags 0x30, under a name that ends with the system time, so it is
// new on every run, since a block freed while mapped and its name outlive the run. Every pair is
// made first, with the main memory it takes, measured to the page the way app_free_sizes does, so
// that no block made later can land where an earlier one was. After each step the test notes its
// result, what the base by UID and the find by address give for both blocks, and the main memory
// the step gives back. The cases:
// - the opened block mapped: the original freed, then the opened block closed while it's mapped
// - both mapped: the original freed, then the opened block closed
// - the opened block mapped and closed, then the original freed
// Then, once all three are done, the unmaps of each case, the opened block's first, and whether a
// block with ENABLE_OPEN can take the original's name after them. They come last, since a wrong
// implementation that freed the memory may crash on them. Last of all, with nothing mapped, the
// original freed, then mapping the opened block's range, which a wrong implementation may take and
// crash on the unmap of, as in gxm_map_no_block, then the close. Nothing reads or writes the
// blocks, and the lines of each step print once it ends.
//
// A find prints 0 when it gives the block, the error when it fails and 1 when it gives another
// UID.
//
// On a PS Vita TV on firmware 3.65 a GPU mapping keeps a block's range, but not the pages of the
// block it shares them with. With only the opened block mapped, freeing the original works and
// gives its 64 KiB back at once, the opened block keeps its UID but its address finds nothing, and
// closing it gives 0x80024703 and drops its UID. With both mapped, the free and the close give
// 0x80024703, the memory stays taken through both unmaps, and the original's name stays taken
// (0x80024E02). An opened block closed while mapped stays without a UID, and freeing the original
// after it still gives the memory back. With nothing mapped, freeing the original gives the memory
// back and the opened block's range no longer maps.
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000
#define PROBES 320
#define MAX_NOTES 96
#define SIZE (64 * KIB)

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

// An original and a block opened from it, and the results of the maps, -1 for a block not mapped
typedef struct Pair {
    const char *label;
    char name[32];
    SceUID original, opened;
    char *original_base, *opened_base;
    int made;
    int original_mapped, opened_mapped;
} Pair;

enum Step { FREE, CLOSE, UNMAP_OPENED, UNMAP_ORIGINAL };

static const struct {
    const char *what;
    const char *when;
} steps[] = {
    { "free the original", "after the free" },
    { "close the opened block", "after the close" },
    { "unmap the opened block", "after the opened block's unmap" },
    { "unmap the original", "after the original's unmap" },
};

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

static char *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

static int find_result(const void *base, SceUID block) {
    const SceUID found = sceKernelFindMemBlockByAddr(base, 0);
    return found == block ? 0 : found < 0 ? found : 1;
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

// The KiB between two measures, or -1 if either failed
static int kib(int before, int after) {
    return before < 0 || after < 0 ? -1 : (before - after) / KIB;
}

// Allocates the original and opens it, and notes the main memory they take
static void make_pair(Pair *pair, const char *label, char letter) {
    pair->label = label;
    pair->made = 0;
    pair->original_mapped = pair->opened_mapped = -1;
    sceClibSnprintf(pair->name, sizeof(pair->name), "gxm pages %c %016llX", letter,
                    sceKernelGetSystemTimeWide());
    const int before = measure();
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    pair->original = sceKernelAllocMemBlock(pair->name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SIZE,
                                            (SceKernelAllocMemBlockOpt *)&opt);
    note(test_created(pair->original), "%s: allocate the original", label);
    if (pair->original < 0)
        return;
    pair->original_base = base_of(pair->original);
    pair->opened = sceKernelOpenMemBlock(pair->name, 0x30);
    note(test_created(pair->opened), "%s: open it with 0x30", label);
    if (pair->opened < 0) {
        sceKernelFreeMemBlock(pair->original);
        return;
    }
    pair->opened_base = base_of(pair->opened);
    pair->made = 1;
    note(kib(before, measure()), "%s: KiB of main memory the two take", label);
}

// Takes the step, then notes its result, what the UIDs and addresses of both blocks give, with
// nothing made since the step, and the KiB of main memory the step gives back
static void step(const Pair *pair, enum Step which) {
    const int before = measure();
    int result;
    switch (which) {
    case FREE:
        result = sceKernelFreeMemBlock(pair->original);
        break;
    case CLOSE:
        result = sceKernelCloseMemBlock(pair->opened);
        break;
    case UNMAP_OPENED:
        result = sceGxmUnmapMemory(pair->opened_base);
        break;
    default:
        result = sceGxmUnmapMemory(pair->original_base);
        break;
    }
    const char *label = pair->label, *when = steps[which].when;
    note(result, "%s: %s", label, steps[which].what);
    void *out = NULL;
    note(sceKernelGetMemBlockBase(pair->original, &out), "%s, %s: the original's base by its UID",
         label, when);
    note(find_result(pair->original_base, pair->original),
         "%s, %s: find the original by its address", label, when);
    note(sceKernelGetMemBlockBase(pair->opened, &out), "%s, %s: the opened block's base by its UID",
         label, when);
    note(find_result(pair->opened_base, pair->opened),
         "%s, %s: find the opened block by its address", label, when);
    note(kib(measure(), before), "%s, %s: KiB of main memory it gives back", label, when);
}

static void map(Pair *pair, int original) {
    if (original) {
        pair->original_mapped = sceGxmMapMemory(pair->original_base, SIZE, RW);
        note(pair->original_mapped, "%s: map the original", pair->label);
    } else {
        pair->opened_mapped = sceGxmMapMemory(pair->opened_base, SIZE, RW);
        note(pair->opened_mapped, "%s: map the opened block", pair->label);
    }
}

// Unmaps what the case mapped, the opened block first, and notes whether a block with ENABLE_OPEN
// can take the original's name after that
static void finish(const Pair *pair) {
    if (pair->opened_mapped == 0)
        step(pair, UNMAP_OPENED);
    if (pair->original_mapped == 0)
        step(pair, UNMAP_ORIGINAL);
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID again = sceKernelAllocMemBlock(pair->name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SIZE,
                                                (SceKernelAllocMemBlockOpt *)&opt);
    note(test_created(again), "%s: allocate a block with the original's name", pair->label);
    if (again >= 0)
        sceKernelFreeMemBlock(again);
    // In case either outlived its end
    sceKernelCloseMemBlock(pair->opened);
    sceKernelFreeMemBlock(pair->original);
    print_notes();
}

int main(void) {
    PRINT("initialize libgxm", initialize());
    // The first measure only warms up
    measure();
    Pair opened_mapped, both_mapped, closed_first, none_mapped;
    make_pair(&opened_mapped, "opened block mapped", 'a');
    make_pair(&both_mapped, "both mapped", 'b');
    make_pair(&closed_first, "opened block closed first", 'c');
    make_pair(&none_mapped, "nothing mapped", 'd');
    print_notes();

    if (opened_mapped.made) {
        map(&opened_mapped, 0);
        step(&opened_mapped, FREE);
        step(&opened_mapped, CLOSE);
        print_notes();
    }
    if (both_mapped.made) {
        map(&both_mapped, 1);
        map(&both_mapped, 0);
        step(&both_mapped, FREE);
        step(&both_mapped, CLOSE);
        print_notes();
    }
    if (closed_first.made) {
        map(&closed_first, 0);
        step(&closed_first, CLOSE);
        step(&closed_first, FREE);
        print_notes();
    }

    // Last, since a wrong implementation that freed the memory may crash on the unmaps
    if (opened_mapped.made)
        finish(&opened_mapped);
    if (both_mapped.made)
        finish(&both_mapped);
    if (closed_first.made)
        finish(&closed_first);
    if (none_mapped.made) {
        step(&none_mapped, FREE);
        print_notes();
        const int mapped = sceGxmMapMemory(none_mapped.opened_base, SIZE, RW);
        note(mapped, "nothing mapped: map the opened block's range");
        if (mapped == 0)
            note(sceGxmUnmapMemory(none_mapped.opened_base), "nothing mapped: unmap it");
        step(&none_mapped, CLOSE);
        finish(&none_mapped);
    }
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
