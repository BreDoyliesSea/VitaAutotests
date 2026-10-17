// sceGxmMapMemory on the memory an app didn't allocate with sceKernelAllocMemBlock, with READ and
// then with READ and WRITE, each map that works unmapped again. No GPU work runs. The ranges are
// whole pages: the page of main, of a const string, of an initialized global and of a zero
// initialized one, the lowest page of the stack of a thread with a stack of 0x4000 that never
// starts, the page of the main thread's TLS slot 0, and the first whole page of a 64 KiB
// allocation from malloc, which is in newlib's heap block. Then the whole blocks that
// sceKernelGetMemBlockInfoByAddr reports for main, the global and that thread's stack, and the
// 1 MiB from the first whole page of a 2 MiB allocation from malloc. A block the info call doesn't
// report prints its error instead. gxm_map_memory maps a page of the main thread's stack.
//
// On a PS Vita TV on firmware 3.65 sceGxmMapMemory refuses the app's code block even with READ
// (0x805B0017, DRIVER), a page of main, a page of a const string and the whole block alike, as it
// refuses a block for code. The pages and the whole blocks of the globals, a new thread's stack,
// the TLS and malloc's heap map with READ and with READ and WRITE.
#include <stdlib.h>

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

static const char text[] = "a const string to map";
static volatile int data_word = 1;
static volatile int bss_word;

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
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

static SceUIntPtr page_of(SceUIntPtr addr) {
    return addr & ~(SceUIntPtr)(4 * KIB - 1);
}

static SceUIntPtr page_after(SceUIntPtr addr) {
    return page_of(addr + 4 * KIB - 1);
}

// Maps the range with READ, then with READ and WRITE, unmapping each map that works
static void map_range(const char *name, SceUIntPtr base, SceSize size) {
    static const struct {
        SceUInt32 attribs;
        const char *name;
    } attribs[] = { { SCE_GXM_MEMORY_ATTRIB_READ, "READ" }, { RW, "READ and WRITE" } };
    for (unsigned int i = 0; i < sizeof(attribs) / sizeof(attribs[0]); i++) {
        const int mapped = sceGxmMapMemory((void *)base, size, attribs[i].attribs);
        PRINTF(mapped, "%s with %s", name, attribs[i].name);
        if (mapped == 0)
            PRINTF(sceGxmUnmapMemory((void *)base), "%s with %s: unmap it", name, attribs[i].name);
    }
}

// Maps the whole block behind addr, as the info call reports it
static void map_block(const char *name, SceUIntPtr addr) {
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    const int ret = sceKernelGetMemBlockInfoByAddr((void *)addr, &info);
    PRINTF(ret, "%s: info", name);
    if (ret == 0)
        map_range(name, (SceUIntPtr)info.mappedBase, info.mappedSize);
}

int main(void) {
    const SceUID thread = sceKernelCreateThread("gxm app memory", entry,
        SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    char *small = malloc(64 * KIB), *big = malloc(2 * MIB);
    const SceUIntPtr code = (SceUIntPtr)main & ~(SceUIntPtr)1;

    PRINT("initialize libgxm", initialize());

    map_range("the page of main", page_of(code), 4 * KIB);
    map_range("the page of a const string", page_of((SceUIntPtr)text), 4 * KIB);
    map_range("the page of an initialized global", page_of((SceUIntPtr)&data_word), 4 * KIB);
    map_range("the page of a zero initialized global", page_of((SceUIntPtr)&bss_word), 4 * KIB);
    map_range("the lowest page of a new thread's stack", (SceUIntPtr)info.stack, 4 * KIB);
    map_range("the page of TLS slot 0", page_of((SceUIntPtr)sceKernelGetTLSAddr(0)), 4 * KIB);
    map_range("a page from malloc", page_after((SceUIntPtr)small), 4 * KIB);

    map_block("the block of main", code);
    map_block("the block of a global", (SceUIntPtr)&data_word);
    map_block("the block of a new thread's stack", (SceUIntPtr)info.stack);
    map_range("1 MiB from malloc", page_after((SceUIntPtr)big), MIB);

    PRINT("terminate libgxm", sceGxmTerminate());
    free(big);
    free(small);
    sceKernelDeleteThread(thread);
    (void)bss_word;
    test_exit();
    return 0;
}
