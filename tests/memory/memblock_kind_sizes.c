// How much free memory a block of each kind takes, to the page. The main free size is measured to
// 4 KiB and the CDRAM one to 256 KiB, the way app_free_sizes does, and the physically contiguous
// one is read as reported, in whole MiB. For each kind: the KiB it takes from each, and whether
// all three are back once it ends. The kinds:
// - USER_RW of 4 KiB and of 12 KiB, 4 KiB that HAS_ALIGNMENT puts on 1 MiB, and 4 KiB and 64 KiB
//   with PHYCONT (0x200000)
// - 4 KiB of USER_RW_UNCACHE, USER_MAIN_GAME_RW, USER_MAIN_DEVICE_RW and USER_MAIN_R
// - 256 KiB of USER_CDRAM_RW and of USER_CDRAM_L1WBWA_RW, and 1 MiB of USER_MAIN_PHYCONT_RW
// - 64 KiB of USER_MAIN_CDIALOG_RW and of USER_CDIALOG_RW
// - a block opened with flags 0x10 and one opened with 0x30 from 64 KiB of USER_RW with
//   ENABLE_OPEN, made while the original lives and ended with sceKernelCloseMemBlock
// - 1 MiB for code from sceKernelAllocMemBlockForVM, ended with sceKernelFreeMemBlockForVM
// - a thread with a stack of 0x1234 and one with 0x4000, made but never started, and ended by the
//   delete
// Everything is measured first and printed after.
//
// On a PS Vita TV on firmware 3.65 a block takes exactly its size from main memory, aligned or not
// and with the PHYCONT attribute or not, for USER_RW, USER_RW_UNCACHE, USER_MAIN_GAME_RW,
// USER_MAIN_DEVICE_RW and USER_MAIN_R. CDRAM blocks take their size from CDRAM,
// USER_MAIN_PHYCONT_RW from physically contiguous memory, and the common dialog types and opened
// blocks take nothing. A 1 MiB block for code takes 1 MiB of main memory. A thread with a stack of
// 0x1234 takes 16 KiB and one with 0x4000 takes 24 KiB, its stack rounded up to 4 KiB and 8 KiB
// more. Every free size is back once each ends.
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000
#define ENABLE_OPEN 0x4000
#define PHYCONT 0x200000
#define PROBES 320
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

// The stubs export it, but no header declares it
int sceKernelFreeMemBlockForVM(SceUID uid);

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

enum How { ALLOC, OPEN, FOR_VM, THREAD };

static const struct {
    const char *name;
    enum How how;
    SceUInt32 type;
    SceSize size;
    SceUInt32 attr;
    // The alignment for HAS_ALIGNMENT, or the open flags
    SceSize extra;
} kinds[] = {
    { "USER_RW, 4 KiB", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, 0, 0 },
    { "USER_RW, 12 KiB", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 12 * KIB, 0, 0 },
    { "USER_RW, 4 KiB aligned to 1 MiB", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB,
      SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT, MIB },
    { "USER_RW, 4 KiB with PHYCONT", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, PHYCONT, 0 },
    { "USER_RW, 64 KiB with PHYCONT", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, PHYCONT,
      0 },
    { "USER_RW_UNCACHE, 4 KiB", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * KIB, 0, 0 },
    { "USER_MAIN_GAME_RW, 4 KiB", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_GAME_RW, 4 * KIB, 0,
      0 },
    { "USER_MAIN_DEVICE_RW, 4 KiB", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_DEVICE_RW, 4 * KIB, 0,
      0 },
    { "USER_MAIN_R, 4 KiB", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R, 4 * KIB, 0, 0 },
    { "USER_CDRAM_RW, 256 KiB", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB, 0, 0 },
    { "USER_CDRAM_L1WBWA_RW, 256 KiB", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_L1WBWA_RW,
      256 * KIB, 0, 0 },
    { "USER_MAIN_PHYCONT_RW, 1 MiB", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, MIB, 0,
      0 },
    { "USER_MAIN_CDIALOG_RW, 64 KiB", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_RW,
      64 * KIB, 0, 0 },
    { "USER_CDIALOG_RW, 64 KiB", ALLOC, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_RW, 64 * KIB, 0, 0 },
    { "opened with 0x10 from 64 KiB", OPEN, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB,
      ENABLE_OPEN, 0x10 },
    { "opened with 0x30 from 64 KiB", OPEN, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB,
      ENABLE_OPEN, 0x30 },
    { "for code, 1 MiB", FOR_VM, 0, MIB, 0, 0 },
    { "a thread with a stack of 0x1234", THREAD, 0, 0x1234, 0, 0 },
    { "a thread with a stack of 0x4000", THREAD, 0, 0x4000, 0, 0 },
};

typedef struct Sizes {
    int main;
    int cdram;
    int phycont;
} Sizes;

typedef struct Result {
    int made;
    Sizes before, with, after;
} Result;

static SceUID probes[PROBES];
static Result results[COUNT(kinds)];

static SceKernelFreeMemorySizeInfo reported(void) {
    SceKernelFreeMemorySizeInfo sizes = { .size = sizeof(sizes) };
    sceKernelGetFreeMemorySize(&sizes);
    return sizes;
}

static int reported_kind(int cdram) {
    const SceKernelFreeMemorySizeInfo sizes = reported();
    return cdram ? sizes.size_cdram : sizes.size_user;
}

// The main or CDRAM free size to the unit of its blocks, or -1 if it never drops
static int measure(int cdram) {
    const SceUInt32 type
        = cdram ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW;
    const SceSize unit = cdram ? 256 * KIB : 4 * KIB;
    const int start = reported_kind(cdram);
    int made = 0, now = start;
    while (made < PROBES && now == start) {
        const SceUID block = sceKernelAllocMemBlock("free probe", type, unit, NULL);
        if (block < 0)
            break;
        probes[made++] = block;
        now = reported_kind(cdram);
    }
    for (int i = 0; i < made; i++)
        sceKernelFreeMemBlock(probes[i]);
    return now == start ? -1 : start + (made - 1) * (int)unit;
}

static Sizes measure_all(void) {
    const Sizes sizes = { measure(0), measure(1), reported().size_phycont };
    return sizes;
}

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

// Makes the kind, ends it, and keeps the free sizes before, with it, and after
static void check(unsigned int i, Result *result) {
    AllocOpt opt = { .size = sizeof(opt), .attr = kinds[i].attr };
    if (kinds[i].attr & SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT)
        opt.alignment = kinds[i].extra;
    SceUID original = -1;
    if (kinds[i].how == OPEN) {
        original = sceKernelAllocMemBlock("kind sizes", kinds[i].type, kinds[i].size,
                                          (SceKernelAllocMemBlockOpt *)&opt);
        if (original < 0) {
            result->made = original;
            return;
        }
    }

    result->before = measure_all();
    SceUID uid;
    switch (kinds[i].how) {
    case ALLOC:
        uid = sceKernelAllocMemBlock("kind sizes", kinds[i].type, kinds[i].size,
                                     (SceKernelAllocMemBlockOpt *)&opt);
        break;
    case OPEN:
        uid = sceKernelOpenMemBlock("kind sizes", (int)kinds[i].extra);
        break;
    case FOR_VM:
        uid = sceKernelAllocMemBlockForVM("kind sizes", kinds[i].size);
        break;
    default:
        uid = sceKernelCreateThread("kind sizes", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER,
                                    kinds[i].size, 0, 0, NULL);
        break;
    }
    result->made = uid;
    result->with = measure_all();
    if (uid >= 0) {
        switch (kinds[i].how) {
        case ALLOC:
            sceKernelFreeMemBlock(uid);
            break;
        case OPEN:
            sceKernelCloseMemBlock(uid);
            break;
        case FOR_VM:
            sceKernelFreeMemBlockForVM(uid);
            break;
        default:
            sceKernelDeleteThread(uid);
            break;
        }
    }
    result->after = measure_all();
    if (original >= 0)
        sceKernelFreeMemBlock(original);
}

// The KiB between two measures, or -1 if either failed
static int kib(int before, int with) {
    return before < 0 || with < 0 ? -1 : (before - with) / KIB;
}

int main(void) {
    // The first measures only warm up
    measure_all();
    for (unsigned int i = 0; i < COUNT(kinds); i++)
        check(i, &results[i]);

    for (unsigned int i = 0; i < COUNT(kinds); i++) {
        const Result *result = &results[i];
        PRINTF(test_created(result->made), "%s: make it", kinds[i].name);
        if (result->made < 0)
            continue;
        PRINTF(kib(result->before.main, result->with.main), "%s: KiB of main memory it takes",
               kinds[i].name);
        PRINTF(kib(result->before.cdram, result->with.cdram), "%s: KiB of CDRAM it takes",
               kinds[i].name);
        PRINTF(kib(result->before.phycont, result->with.phycont),
               "%s: KiB of physically contiguous memory it takes", kinds[i].name);
        PRINTF(result->before.main >= 0 && result->before.main == result->after.main
                   && result->before.cdram == result->after.cdram
                   && result->before.phycont == result->after.phycont,
               "%s: the free sizes are back once it ends", kinds[i].name);
    }
    test_exit();
    return 0;
}
