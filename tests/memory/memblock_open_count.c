// How many times one block may be open at once with sceKernelOpenMemBlock. The block is 4 KiB of
// USER_RW with ENABLE_OPEN holding a known word, opened with flags 0x10 up to 128 times or until
// an open fails: how many opens work and the error of the one that fails, whether every opened
// block has its own UID and its own address, and whether each one reads the word. Then once they
// are all closed, the same with 0x30, to see whether closing gives the opens back. The output is
// only printed once the opened blocks are closed.
//
// On a PS Vita TV on firmware 3.65 all 128 opens work with each flag, each a block of its own at
// its own address that reads the original's word and closes.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000
#define MAX_OPENS 128
#define WORD 0x600DCAFE

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

typedef struct Opened {
    SceUID uid;
    volatile SceUInt32 *base;
} Opened;

static Opened opened[MAX_OPENS];

static volatile SceUInt32 *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

// Opens the block with the flags until an open fails, checks them all, closes them and prints
static void open_all(int flags) {
    int count = 0;
    SceUID error = 0;
    while (count < MAX_OPENS) {
        const SceUID uid = sceKernelOpenMemBlock("open count", flags);
        if (uid < 0) {
            error = uid;
            break;
        }
        opened[count].uid = uid;
        opened[count].base = base_of(uid);
        count++;
    }

    int unique_uids = 1, unique_bases = 1, all_read = 1;
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            unique_uids &= opened[i].uid != opened[j].uid;
            unique_bases &= opened[i].base != opened[j].base;
        }
        // Only a block the find still gives is read
        const int found = opened[i].base
            && sceKernelFindMemBlockByAddr((void *)opened[i].base, 0) == opened[i].uid;
        all_read &= found && opened[i].base[0] == WORD;
    }
    int closed = 0;
    for (int i = 0; i < count; i++)
        closed += sceKernelCloseMemBlock(opened[i].uid) == 0;

    PRINTF(count, "flags 0x%X: opens that work", flags);
    PRINTF(error, "flags 0x%X: the error of the one that fails", flags);
    PRINTF(unique_uids, "flags 0x%X: each has its own UID", flags);
    PRINTF(unique_bases, "flags 0x%X: each has its own address", flags);
    PRINTF(all_read, "flags 0x%X: each reads the word", flags);
    PRINTF(closed == count, "flags 0x%X: each closes", flags);
}

int main(void) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID original = sceKernelAllocMemBlock(
        "open count", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("allocate the original", test_created(original));
    if (original < 0) {
        test_exit();
        return 0;
    }
    base_of(original)[0] = WORD;
    open_all(0x10);
    open_all(0x30);
    PRINT("free the original", sceKernelFreeMemBlock(original));
    test_exit();
    return 0;
}
