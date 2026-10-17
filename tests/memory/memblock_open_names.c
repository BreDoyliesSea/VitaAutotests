// Which block sceKernelOpenMemBlock opens by name. Each block is 64 KiB of USER_RW holding a word
// of its own, and an open with flags 0x10 shows which block it maps by that word: 1 for the first
// one made, 2 for the second, 0 for neither. The cases:
// - two blocks with ENABLE_OPEN and the same name: whether the second is made, which one an open
//   gives, which one once the first is freed, and opening the name once both are freed
// - a block without ENABLE_OPEN and one with it under the same name, made in both orders
// - a block with a 31 character name, opened by that name, by it and one more character, and by
//   its first 30 characters. Then a block with a 32 character name, opened by that name, by its
//   first 31 characters, and by it and one more character.
// - a block with an empty name, opened by the empty name
// - the name an opened block gets, SceClient: and the original's name, while the opened block
//   lives
//
// On a PS Vita TV on firmware 3.65 a second ENABLE_OPEN block with the name of one that lives fails
// with 0x80024E02 (DUPLICATE_NAME). A block without ENABLE_OPEN may take any name, and opening the
// name gives the ENABLE_OPEN block, whichever was made first. Only the first 31 characters of a
// name count, at both ends, an empty name works, and the "SceClient:" name of an opened block
// doesn't open (0x8002450A).
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000
#define FIRST_WORD 0x11111111
#define SECOND_WORD 0x22222222

#define NAME_30 "abcdefghijklmnopqrstuvwxyz0123"
#define NAME_31 NAME_30 "4"
#define NAME_32 NAME_31 "5"
#define NAME_33 NAME_32 "6"

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

// Makes a block with the name and attr holding word
static SceUID make(const char *name, SceUInt32 attr, SceUInt32 word) {
    AllocOpt opt = { .size = sizeof(opt), .attr = attr };
    const SceUID block = sceKernelAllocMemBlock(
        name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    if (block >= 0)
        base_of(block)[0] = word;
    return block;
}

// Opens the name with 0x10 and prints the result and which block it maps
static void open_which(const char *label, const char *name) {
    const SceUID opened = sceKernelOpenMemBlock(name, 0x10);
    PRINTF(test_created(opened), "%s", label);
    if (opened < 0)
        return;
    volatile SceUInt32 *theirs = base_of(opened);
    int which = 0;
    if (theirs && sceKernelFindMemBlockByAddr((void *)theirs, 0) == opened) {
        if (theirs[0] == FIRST_WORD)
            which = 1;
        else if (theirs[0] == SECOND_WORD)
            which = 2;
    }
    PRINTF(which, "%s: the block it maps", label);
    sceKernelCloseMemBlock(opened);
}

// Makes a block with attrs first and second under one name, and opens the name
static void pair(const char *label, const char *name, SceUInt32 first, SceUInt32 second) {
    char text[96];
    const SceUID a = make(name, first, FIRST_WORD);
    PRINTF(test_created(a), "%s: make the first", label);
    const SceUID b = make(name, second, SECOND_WORD);
    PRINTF(test_created(b), "%s: make the second", label);
    sceClibSnprintf(text, sizeof(text), "%s: open the name", label);
    open_which(text, name);
    if (a >= 0)
        sceKernelFreeMemBlock(a);
    sceClibSnprintf(text, sizeof(text), "%s: open the name once the first is freed", label);
    open_which(text, name);
    if (b >= 0)
        sceKernelFreeMemBlock(b);
    sceClibSnprintf(text, sizeof(text), "%s: open the name once both are freed", label);
    open_which(text, name);
}

int main(void) {
    pair("both with ENABLE_OPEN", "names twin", ENABLE_OPEN, ENABLE_OPEN);
    pair("the first without ENABLE_OPEN", "names plain first", 0, ENABLE_OPEN);
    pair("the second without ENABLE_OPEN", "names plain second", ENABLE_OPEN, 0);

    SceUID block = make(NAME_31, ENABLE_OPEN, FIRST_WORD);
    PRINT("31 characters: make it", test_created(block));
    open_which("31 characters: open it by its name", NAME_31);
    open_which("31 characters: open it by its name and one more character", NAME_32);
    open_which("31 characters: open it by its first 30 characters", NAME_30);
    if (block >= 0)
        sceKernelFreeMemBlock(block);

    block = make(NAME_32, ENABLE_OPEN, FIRST_WORD);
    PRINT("32 characters: make it", test_created(block));
    open_which("32 characters: open it by its name", NAME_32);
    open_which("32 characters: open it by its first 31 characters", NAME_31);
    open_which("32 characters: open it by its name and one more character", NAME_33);
    if (block >= 0)
        sceKernelFreeMemBlock(block);

    block = make("", ENABLE_OPEN, FIRST_WORD);
    PRINT("an empty name: make it", test_created(block));
    open_which("an empty name: open it", "");
    if (block >= 0)
        sceKernelFreeMemBlock(block);

    block = make("client", ENABLE_OPEN, FIRST_WORD);
    PRINT("an opened block's name: make the original", test_created(block));
    const SceUID opened = block >= 0 ? sceKernelOpenMemBlock("client", 0x30) : block;
    PRINT("an opened block's name: open it", test_created(opened));
    open_which("an opened block's name: open SceClient:client", "SceClient:client");
    if (opened >= 0)
        sceKernelCloseMemBlock(opened);
    if (block >= 0)
        sceKernelFreeMemBlock(block);

    test_exit();
    return 0;
}
