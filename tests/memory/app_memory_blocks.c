// The memory blocks behind the memory an app didn't allocate with sceKernelAllocMemBlock, one
// address each: main, another function, malloc, which newlib links into the app, _init, the code's
// first function, two const strings, two initialized globals and two zero initialized ones, a local
// on the main thread's stack and one on a new thread's stack, TLS slot 0 of the main thread, two
// allocations from malloc, which come from newlib's heap block, and the stubs of
// sceKernelGetTLSAddr and sceKernelFindMemBlockByAddr. For each: what sceKernelFindMemBlockByAddr
// gives, and for a UID it gives, whether sceKernelGetMemBlockBase gives the block's base, then what
// sceKernelGetMemBlockInfoByAddr reports: whether the block holds the address, its type, memory
// type, access and region, and whether its base and size are 4 KiB aligned. Then which of them
// share a block, and whether a thread's local is in the block of the stack base its thread info
// reports. Then whether the code block starts at _init, and the data block at the data's first
// byte, __init_array_start. Where the data block ends, and the code block's alignment, change with
// the build, so they aren't printed. Last, what each stub holds, a jump to a library function or a
// system call, and for a jump, the lookups for the function it goes to.
//
// A find prints 0 when it gives a UID and the error when it fails. Addresses are printed as the
// 256 MiB region of a block's base, alignments and comparisons, and no block's size is printed,
// since the sizes of the code and data blocks change with the build.
//
// On a PS Vita TV on firmware 3.65 no address here but malloc's memory gives a UID: the find gives
// 0x80024501 (INVALID_UID) for the code, the const strings, the stubs, the globals, both stacks and
// the TLS, while the info call reports each block. The code, the const strings and the stubs share
// one block of type 0x0C20D050, access R, which starts at _init. The initialized and zero
// initialized globals share one block of type 0x0C20D060, access R and W, which starts at
// __init_array_start. Each stack and the TLS are blocks of their own, of type 0x0C20D060. All are
// in region 0x8 and 4 KiB aligned. malloc's memory comes from newlib's heap, a USER_RW block with a
// UID and a size of whole MiB, which both allocations share. The stub of sceKernelGetTLSAddr jumps
// to library code in a block of type 0x0320D050 in region 0xE, outside the app's code block, and
// the stub of sceKernelFindMemBlockByAddr is a system call.
#include <stdlib.h>

#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

// Linker symbols: the code's first function, and the data's first byte and its end
extern void _init(void);
extern char __init_array_start[];

typedef struct Found {
    SceUID uid;
    int find;
    int info;
    SceKernelMemBlockInfo block;
} Found;

static const char text_a[] = "the first const string";
static const char text_b[] = "the second const string";
static volatile int data_a = 0x1234, data_b = 0x5678;
static volatile int bss_a, bss_b;
static SceUIntPtr thread_local_address;
static Found thread_local_found, thread_stack_found;

static SceUIntPtr code_address(const void *function) {
    return (SceUIntPtr)function & ~(SceUIntPtr)1;
}

static Found look_up(SceUIntPtr addr) {
    Found found = { .block = { .size = sizeof(found.block) } };
    found.uid = sceKernelFindMemBlockByAddr((const void *)addr, 0);
    found.find = found.uid < 0 ? found.uid : 0;
    found.info = sceKernelGetMemBlockInfoByAddr((void *)addr, &found.block);
    return found;
}

static SceUIntPtr base_of(const Found *found) {
    return (SceUIntPtr)found->block.mappedBase;
}

static SceUIntPtr end_of(const Found *found) {
    return base_of(found) + found->block.mappedSize;
}

static int same_block(const Found *a, const Found *b) {
    return a->info == 0 && b->info == 0 && base_of(a) == base_of(b);
}

static void print_found(const char *name, SceUIntPtr addr, const Found *found) {
    PRINTF(found->find, "%s: find", name);
    if (found->uid >= 0) {
        void *base = NULL;
        PRINTF(sceKernelGetMemBlockBase(found->uid, &base), "%s: the base of the UID", name);
        PRINTF(base == found->block.mappedBase, "%s: that base is the mapped base", name);
    }
    PRINTF(found->info, "%s: info", name);
    if (found->info < 0)
        return;
    PRINTF(addr >= base_of(found) && addr < end_of(found), "%s: the block holds it", name);
    PRINTF(found->block.type, "%s: its type", name);
    PRINTF(found->block.memoryType, "%s: its memory type", name);
    PRINTF(found->block.access, "%s: its access", name);
    PRINTF(base_of(found) >> 28, "%s: the region of its base", name);
    PRINTF(base_of(found) % (4 * KIB) == 0, "%s: its base is 4 KiB aligned", name);
    PRINTF(found->block.mappedSize % (4 * KIB) == 0, "%s: its size is a multiple of 4 KiB", name);
}

static void check(const char *name, SceUIntPtr addr, Found *found) {
    *found = look_up(addr);
    print_found(name, addr, found);
}

static int stack_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    volatile int local = 0;
    thread_local_address = (SceUIntPtr)&local;
    thread_local_found = look_up(thread_local_address);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    thread_stack_found = look_up((SceUIntPtr)info.stack);
    return local;
}

static __attribute__((noinline)) int other_function(int value) {
    return value * 3 + 1;
}

// What a stub holds: 1 for MOVW, MOVT and BX of r12, a jump, 2 for MOVW of r12 and SVC 0, a
// system call, and 0 for anything else. *target gets where a jump goes.
static int stub_kind(const void *stub, SceUIntPtr *target) {
    const volatile SceUInt32 *words = (const volatile SceUInt32 *)((SceUIntPtr)stub & ~3u);
    *target = 0;
    if ((words[0] & 0xFFF0F000) != 0xE300C000)
        return 0;
    if (words[1] == 0xEF000000)
        return 2;
    if ((words[1] & 0xFFF0F000) != 0xE340C000 || words[2] != 0xE12FFF1C)
        return 0;
    const SceUInt32 low = ((words[0] >> 4) & 0xF000) | (words[0] & 0xFFF);
    const SceUInt32 high = ((words[1] >> 4) & 0xF000) | (words[1] & 0xFFF);
    *target = (high << 16) | low;
    return 1;
}

static void check_stub(const char *name, const void *stub, const Found *code) {
    SceUIntPtr target;
    PRINTF(stub_kind(stub, &target), "%s: what the stub holds", name);
    if (!target)
        return;
    char label[96];
    sceClibSnprintf(label, sizeof(label), "%s: the function the stub jumps to", name);
    Found found;
    check(label, target & ~(SceUIntPtr)1, &found);
    PRINTF(same_block(&found, code), "%s: in the code block", label);
}

int main(void) {
    Found code, init, other, malloc_code, string_a, string_b, global_a, global_b, zero_a, zero_b,
        main_local, tls, heap_a, heap_b, stub_tls, stub_find;

    check("main", code_address((const void *)main), &code);
    check("another function", code_address((const void *)other_function), &other);
    check("malloc", code_address((const void *)malloc), &malloc_code);
    check("_init", code_address((const void *)_init), &init);
    check("a const string", (SceUIntPtr)text_a, &string_a);
    check("another const string", (SceUIntPtr)text_b, &string_b);
    check("the stub of sceKernelGetTLSAddr", code_address((const void *)sceKernelGetTLSAddr),
        &stub_tls);
    check("the stub of sceKernelFindMemBlockByAddr",
        code_address((const void *)sceKernelFindMemBlockByAddr), &stub_find);

    check("an initialized global", (SceUIntPtr)&data_a, &global_a);
    check("another initialized global", (SceUIntPtr)&data_b, &global_b);
    check("a zero initialized global", (SceUIntPtr)&bss_a, &zero_a);
    check("another zero initialized global", (SceUIntPtr)&bss_b, &zero_b);

    volatile int local = 0;
    check("a local on the main thread's stack", (SceUIntPtr)&local, &main_local);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    const Found main_stack = look_up((SceUIntPtr)info.stack);
    const SceUID thread = test_start_thread(stack_thread);
    test_join_thread(thread);
    print_found("a local on a new thread's stack", thread_local_address, &thread_local_found);
    check("TLS slot 0 of the main thread", (SceUIntPtr)sceKernelGetTLSAddr(0), &tls);

    void *first = malloc(64), *second = malloc(64);
    check("memory from malloc", (SceUIntPtr)first, &heap_a);
    check("another allocation from malloc", (SceUIntPtr)second, &heap_b);
    if (heap_a.info == 0)
        PRINT("memory from malloc: its block's size is a whole number of MiB",
            heap_a.block.mappedSize % MIB == 0);

    PRINT("main and another function share a block", same_block(&code, &other));
    PRINT("main and malloc share a block", same_block(&code, &malloc_code));
    PRINT("main and _init share a block", same_block(&code, &init));
    PRINT("main and a const string share a block", same_block(&code, &string_a));
    PRINT("the two const strings share a block", same_block(&string_a, &string_b));
    PRINT("main and the stubs share a block",
        same_block(&code, &stub_tls) && same_block(&code, &stub_find));
    PRINT("the two initialized globals share a block", same_block(&global_a, &global_b));
    PRINT("the two zero initialized globals share a block", same_block(&zero_a, &zero_b));
    PRINT("an initialized and a zero initialized global share a block",
        same_block(&global_a, &zero_a));
    PRINT("main and a global share a block", same_block(&code, &global_a));
    PRINT("the two allocations from malloc share a block", same_block(&heap_a, &heap_b));
    PRINT("an allocation from malloc and a global share a block", same_block(&heap_a, &global_a));
    PRINT("the main thread's local and the TLS share a block", same_block(&main_local, &tls));
    PRINT("the main thread's local is in the block of its stack base",
        same_block(&main_local, &main_stack));
    PRINT("the new thread's local is in the block of its stack base",
        same_block(&thread_local_found, &thread_stack_found));
    PRINT("the two threads' locals share a block", same_block(&main_local, &thread_local_found));

    if (code.info == 0)
        PRINT("the code block starts at _init",
            base_of(&code) == code_address((const void *)_init));
    const Found data_start = look_up((SceUIntPtr)__init_array_start);
    PRINT("the data's first byte is in the block of a global", same_block(&data_start, &global_a));
    if (global_a.info == 0)
        PRINT("the data block starts at the data's first byte",
            base_of(&global_a) == (SceUIntPtr)__init_array_start);

    check_stub("sceKernelGetTLSAddr", (const void *)sceKernelGetTLSAddr, &code);
    check_stub("sceKernelFindMemBlockByAddr", (const void *)sceKernelFindMemBlockByAddr, &code);

    free(second);
    free(first);
    (void)local;
    test_exit();
    return 0;
}
