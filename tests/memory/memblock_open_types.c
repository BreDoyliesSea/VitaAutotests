// sceKernelOpenMemBlock on a block of each type an app may allocate with ENABLE_OPEN, with flags
// 0x10 and with 0x30: the result, and for a block it opens its type, memory type, access, which 256
// MiB region its base is in, whether its mapped size is the original's, and whether it reads a word
// the original wrote. Then CDRAM blocks of 1 MiB and 8 MiB, to see whether the size matters.
//
// On a PS Vita TV on firmware 3.65 USER_RW, USER_MAIN_R, USER_RW_UNCACHE and the four common dialog
// types open, each as a new block in the original's region that reads its memory. 0x10 opens one
// read only, with the read only version of its type, and 0x30 read and write, so a read only type
// opens only with 0x10 (0x80024B0C otherwise) and USER_RW_UNCACHE only with 0x30, since its read
// only version isn't a type (0x80024B01). USER_CDRAM_RW fails with 0x80024804 at every size, and so
// does USER_CDRAM_R with 0x10 (with 0x30 it gets 0x80024B0C like any read only type). The other
// types fail with 0x80024B01.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000
#define ENABLE_OPEN 0x4000

// The option struct games pass, without the fields vitasdk adds after strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static const struct {
    SceUInt32 type;
    SceSize unit;
} types[] = {
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_L1WBWA_RW, 256 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_R, 256 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_DEVICE_RW, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_GAME_RW, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, MIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, MIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_RW, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_NC_RW, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_R, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_RW, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_NC_R, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_NC_RW, 4 * KIB },
};

static void *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

// Allocates an openable block of the type and size, and opens it with each flag
static void open_type(SceUInt32 type, SceSize size) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID original = sceKernelAllocMemBlock("open type", type, size, (SceKernelAllocMemBlockOpt *)&opt);
    PRINTF(test_created(original), "type 0x%08X, 0x%X bytes: allocate it", (unsigned)type, (unsigned)size);
    if (original < 0)
        return;
    volatile SceUInt32 *mine = base_of(original);
    // Only a type an app may write to gets a marker
    const int writable = (type & 0x20) != 0;
    if (writable)
        mine[0] = 0x13579BDF;

    static const int flags[] = { 0x10, 0x30 };
    for (unsigned int i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        const SceUID opened = sceKernelOpenMemBlock("open type", flags[i]);
        PRINTF(test_created(opened), "type 0x%08X, 0x%X bytes: open it with 0x%X", (unsigned)type, (unsigned)size, flags[i]);
        if (opened < 0)
            continue;
        volatile SceUInt32 *theirs = base_of(opened);
        SceKernelMemBlockInfo info = { .size = sizeof(info) };
        sceKernelGetMemBlockInfoByAddr((void *)theirs, &info);
        PRINTF(info.type, "type 0x%08X, 0x%X bytes, 0x%X: its type", (unsigned)type, (unsigned)size, flags[i]);
        PRINTF(info.memoryType, "type 0x%08X, 0x%X bytes, 0x%X: its memory type", (unsigned)type, (unsigned)size, flags[i]);
        PRINTF(info.access, "type 0x%08X, 0x%X bytes, 0x%X: its access", (unsigned)type, (unsigned)size, flags[i]);
        PRINTF((SceUIntPtr)theirs >> 28, "type 0x%08X, 0x%X bytes, 0x%X: its region", (unsigned)type, (unsigned)size, flags[i]);
        PRINTF(info.mappedSize == size, "type 0x%08X, 0x%X bytes, 0x%X: its mapped size is the original's", (unsigned)type, (unsigned)size, flags[i]);
        if (writable)
            PRINTF(theirs[0] == 0x13579BDF, "type 0x%08X, 0x%X bytes, 0x%X: it reads the original's word", (unsigned)type, (unsigned)size, flags[i]);
        sceKernelCloseMemBlock(opened);
    }
    sceKernelFreeMemBlock(original);
}

int main(void) {
    for (unsigned int i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        open_type(types[i].type, types[i].unit);
    open_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, MIB);
    open_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 8 * MIB);
    test_exit();
    return 0;
}
