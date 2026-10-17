// The memory blocks behind the TLS. For the main thread: what sceKernelFindMemBlockByAddr gives
// and sceKernelGetMemBlockInfoByAddr reports for TLS slot 0, its block's size, type, memory type,
// access and region, the offset of slot 0 into the block and the offset TPIDRURO points to,
// whether slot 255 and TPIDRURO are in the same block, and whether the thread's stack base or a
// local on its stack is. Then the same, without TPIDRURO and the local, for a thread with a stack
// of 0x4000 before it starts, from sceKernelGetThreadTLSAddr, and whether its TLS block is the main
// thread's. Then while it waits: whether the slot 0 it sees is the one it had before it started,
// and TPIDRURO 0x800 past it. Last, once it ended, the info call on its old slot 0, then the same
// right after the delete, with nothing made in between.
//
// A find prints 0 when it gives a UID and the error when it fails. Comparisons with a block print
// 0 when the info call failed.
//
// On a PS Vita TV on firmware 3.65 each thread's TLS is a 4 KiB block of its own of type
// 0x0C20D060, access R and W, in region 0x8, with no UID (the find gives 0x80024501), slot 0 at its
// start, slot 255 in it too, and TPIDRURO 0x800 into it. It is neither the stack's block nor
// another thread's. A new thread has it before it starts and keeps it while it runs and after it
// ends, and the delete ends it (0x80022007).
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "registers.h"
#include "test.h"
#include "threadmgr_extra.h"

typedef struct Found {
    int find;
    int info;
    SceKernelMemBlockInfo block;
} Found;

static SceUID sema;
static volatile SceUIntPtr seen_slot, seen_tpidruro;

static Found look_up(SceUIntPtr addr) {
    Found found = { .block = { .size = sizeof(found.block) } };
    const SceUID uid = sceKernelFindMemBlockByAddr((const void *)addr, 0);
    found.find = uid < 0 ? uid : 0;
    found.info = sceKernelGetMemBlockInfoByAddr((void *)addr, &found.block);
    return found;
}

static int same_block(const Found *a, const Found *b) {
    return a->info == 0 && b->info == 0 && a->block.mappedBase == b->block.mappedBase;
}

static int info_error(SceUIntPtr addr) {
    SceKernelMemBlockInfo block = { .size = sizeof(block) };
    return sceKernelGetMemBlockInfoByAddr((void *)addr, &block);
}

// Prints what the calls report about a thread's TLS block, and returns the block of slot 0
static Found print_tls(const char *name, SceUIntPtr slot0, SceUIntPtr slot255, SceUIntPtr tpidruro,
    SceUIntPtr stack, SceUIntPtr local) {
    const Found found = look_up(slot0);
    PRINTF(found.find, "%s: find by slot 0", name);
    PRINTF(found.info, "%s: info by slot 0", name);
    if (found.info == 0) {
        const SceUIntPtr base = (SceUIntPtr)found.block.mappedBase;
        PRINTF(found.block.mappedSize, "%s: its size", name);
        PRINTF(found.block.type, "%s: its type", name);
        PRINTF(found.block.memoryType, "%s: its memory type", name);
        PRINTF(found.block.access, "%s: its access", name);
        PRINTF(base >> 28, "%s: the region of its base", name);
        PRINTF(slot0 - base, "%s: the offset of slot 0 into it", name);
        if (tpidruro)
            PRINTF(tpidruro - base, "%s: the offset TPIDRURO points to", name);
    }
    const Found last = look_up(slot255), base = look_up(stack);
    PRINTF(same_block(&found, &last), "%s: slot 255 is in the block of slot 0", name);
    if (tpidruro) {
        const Found pointed = look_up(tpidruro);
        PRINTF(same_block(&found, &pointed), "%s: TPIDRURO is in the block of slot 0", name);
    }
    PRINTF(same_block(&found, &base), "%s: the stack base is in the block of slot 0", name);
    if (local) {
        const Found on_stack = look_up(local);
        PRINTF(same_block(&found, &on_stack), "%s: a local is in the block of slot 0", name);
    }
    return found;
}

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    seen_slot = (SceUIntPtr)sceKernelGetTLSAddr(0);
    seen_tpidruro = get_tpidruro();
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    return 0;
}

int main(void) {
    sema = sceKernelCreateSema("tls block", 0, 0, 1, NULL);

    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    volatile int local = 0;
    const Found main_tls = print_tls("the main thread", (SceUIntPtr)sceKernelGetTLSAddr(0),
        (SceUIntPtr)sceKernelGetTLSAddr(255), get_tpidruro(), (SceUIntPtr)info.stack,
        (SceUIntPtr)&local);

    const SceUID thread = sceKernelCreateThread("tls block", waiter,
        SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    SceKernelThreadInfo thread_info = { .size = sizeof(thread_info) };
    sceKernelGetThreadInfo(thread, &thread_info);
    const SceUIntPtr slot0 = (SceUIntPtr)sceKernelGetThreadTLSAddr(thread, 0);
    PRINT("a new thread before it starts: slot 0 has an address", slot0 != 0);
    const Found before = print_tls("a new thread before it starts", slot0,
        (SceUIntPtr)sceKernelGetThreadTLSAddr(thread, 255), 0, (SceUIntPtr)thread_info.stack, 0);
    PRINT("a new thread before it starts: its TLS block is the main thread's",
        same_block(&before, &main_tls));

    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema));
    PRINT("the new thread while it waits: its slot 0 is the one it had before it started",
        seen_slot == slot0);
    PRINT("the new thread while it waits: TPIDRURO is 0x800 past it",
        seen_tpidruro == slot0 + 0x800);
    sceKernelSignalSema(sema, 1);
    sceKernelWaitThreadEnd(thread, NULL, NULL);

    PRINT("the new thread once it ended: info by its old slot 0", info_error(slot0));
    const int deleted = sceKernelDeleteThread(thread);
    const int after = info_error(slot0);
    PRINT("the new thread: delete", deleted);
    PRINT("the new thread: info by its old slot 0 after the delete", after);

    (void)local;
    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}
