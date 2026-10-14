// sceKernelCreateThread with an option struct: which sizes it accepts, and which bits of its attr
// field, with the other fields left at 0. Then stack sizes that aren't page multiples: what thread
// info reports as the stack size, whether the stack is page aligned, and whether the thread runs.
// Threads made with option bits are only created, never started.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

// Big enough for any size the test claims, all zero but for what a case sets
static union {
    SceKernelThreadOptParam opt;
    unsigned char bytes[0x1000];
} buf;

// Creates a thread with the option struct in buf and returns 0 or the error
static int create_with_opt(void) {
    const SceUID thread = sceKernelCreateThread("opt", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, &buf.opt);
    if (thread < 0)
        return thread;
    sceKernelDeleteThread(thread);
    return 0;
}

int main(void) {
    static const SceSize sizes[] = { 0, 3, 4, 8, 0x14, 0x18, 0x1C, 0x20, 0x1000 };
    for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        sceClibMemset(&buf, 0, sizeof(buf));
        buf.opt.size = sizes[i];
        PRINTF(create_with_opt(), "an option struct of size 0x%X", (unsigned)sizes[i]);
    }

    for (int bit = 0; bit < 32; bit++) {
        sceClibMemset(&buf, 0, sizeof(buf));
        buf.opt.size = 0x14;
        buf.opt.attr = 1u << bit;
        PRINTF(create_with_opt(), "option attr 0x%08X with the other fields 0", 1u << bit);
    }

    // Stack sizes
    static const int stacks[] = { 1, 0x10, 0xFF, 0x100, 0x200, 0x800, 0xFFF, 0x1000, 0x1001, 0x1800, 0x3FFF, 0x4000, 0x4001, 0x100000 };
    for (unsigned int i = 0; i < sizeof(stacks) / sizeof(stacks[0]); i++) {
        const SceUID thread = sceKernelCreateThread("stack", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, stacks[i], 0, 0, NULL);
        PRINTF(test_created(thread), "a stack of 0x%X: create", stacks[i]);
        if (thread < 0)
            continue;
        SceKernelThreadInfo info = { .size = sizeof(info) };
        sceKernelGetThreadInfo(thread, &info);
        PRINTF(info.stackSize, "a stack of 0x%X: the size in its info", stacks[i]);
        PRINTF(((SceUIntPtr)info.stack & 0xFFF) == 0, "a stack of 0x%X: the stack is page aligned", stacks[i]);
        // Only a stack reported as a page or more is used, in case the size is taken as given
        if (info.stackSize >= 0x1000) {
            sceKernelStartThread(thread, 0, NULL);
            SceUInt timeout = TEST_WAIT_US;
            PRINTF(sceKernelWaitThreadEnd(thread, NULL, &timeout), "a stack of 0x%X: it runs to its end", stacks[i]);
        }
        sceKernelDeleteThread(thread);
    }

    test_exit();
    return 0;
}
