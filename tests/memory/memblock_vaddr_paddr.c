// Whether an app can choose where a memory block goes, its virtual address with attr 0x1 or its
// physical address with HAS_PADDR (0x2). The requested virtual address X is 16 MiB past where a
// freed 64 KiB block was, rounded up to 1 MiB, where a plain allocation doesn't go. Each case asks
// for 64 KiB of USER_RW:
// - a plain block, to show where blocks go without a request
// - attr 0x1 with option size 0x14, then with option size 0x40 and X in words 5 to 14, the words
//   after strBaseBlockName but the last, which must be 0, and if that block lands at X, X in each
//   of those words alone
// - attr 0x1 asking for an address inside a live block
// - HAS_PADDR with option size 0x14, then with option size 0x40 and a main memory physical address
//   in words 5 to 14. A block it makes is freed without being touched.
//
// On a PS Vita TV on firmware 3.65 both attrs fail with 0x80020005
// (SCE_KERNEL_ERROR_INVALID_ARGUMENT) with either option size, so the kernel always chooses where
// an app's block goes.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define HAS_VADDR 0x1
#define HAS_PADDR 0x2
// Words 5 to 14. Word 15, the last of a 0x40 byte option, stays 0, since anything else there fails
#define EXTRA_WORDS 10

typedef struct Opt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUID baseBlock;
    const char *baseBlockName;
    SceUInt32 extra[EXTRA_WORDS];
    SceUInt32 last;
} Opt;

static SceUIntPtr base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return (SceUIntPtr)base;
}

// Allocates 64 KiB with the option, returns its UID or the error and writes its base to *base
static SceUID alloc_with(const Opt *opt, SceUIntPtr *base) {
    const SceUID block = sceKernelAllocMemBlock("placed", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, (SceKernelAllocMemBlockOpt *)opt);
    *base = block >= 0 ? base_of(block) : 0;
    return block;
}

// Prints the result of an allocation with the option and whether it lands at want
static int try_at(const char *label, const Opt *opt, SceUIntPtr want) {
    SceUIntPtr base;
    const SceUID block = alloc_with(opt, &base);
    PRINTF(test_created(block), "%s", label);
    const int there = block >= 0 && base == want;
    if (block >= 0) {
        PRINTF(there, "%s: it lands at the requested address", label);
        sceKernelFreeMemBlock(block);
    }
    return there;
}

static void fill_extra(Opt *opt, SceUInt32 value) {
    for (int i = 0; i < EXTRA_WORDS; i++)
        opt->extra[i] = value;
}

int main(void) {
    const SceUID freed = sceKernelAllocMemBlock("freed", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, NULL);
    const SceUIntPtr freed_base = base_of(freed);
    sceKernelFreeMemBlock(freed);
    const SceUIntPtr want = (freed_base + 16 * MIB + MIB - 1) & ~(SceUIntPtr)(MIB - 1);

    Opt opt;
    sceClibMemset(&opt, 0, sizeof(opt));
    opt.size = 0x14;
    try_at("a plain block", &opt, want);

    opt.attr = HAS_VADDR;
    try_at("attr 0x1, option size 0x14", &opt, want);
    opt.size = sizeof(opt);
    fill_extra(&opt, want);
    if (try_at("attr 0x1, the address in words 5 to 14", &opt, want)) {
        char label[64];
        for (int i = 0; i < EXTRA_WORDS; i++) {
            fill_extra(&opt, 0);
            opt.extra[i] = want;
            sceClibSnprintf(label, sizeof(label), "attr 0x1, the address in word %d", i + 5);
            try_at(label, &opt, want);
        }
    }

    // An address inside a live block
    const SceUID live = sceKernelAllocMemBlock("live", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, NULL);
    const SceUIntPtr inside = base_of(live) + 16 * KIB;
    fill_extra(&opt, inside);
    try_at("attr 0x1, an address inside a live block", &opt, inside);
    sceKernelFreeMemBlock(live);

    sceClibMemset(&opt, 0, sizeof(opt));
    opt.size = 0x14;
    opt.attr = HAS_PADDR;
    SceUID phys = sceKernelAllocMemBlock("phys", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("HAS_PADDR, option size 0x14", test_created(phys));
    if (phys >= 0)
        sceKernelFreeMemBlock(phys);
    opt.size = sizeof(opt);
    fill_extra(&opt, 0x41000000);
    phys = sceKernelAllocMemBlock("phys", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("HAS_PADDR, a physical address in words 5 to 14", test_created(phys));
    if (phys >= 0)
        sceKernelFreeMemBlock(phys);
    test_exit();
    return 0;
}
