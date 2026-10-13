// Which attribute bits sceKernelChangeCurrentThreadAttr sets and clears, one bit at a time, and
// the attributes thread info reports after each change. A thread of its own does the changes.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

static volatile int set_result[32], attr_after_set[32], clear_result[32], attr_after_clear[32];

static int attr_of_self(void) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
    return info.attr;
}

static int changing_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (int bit = 0; bit < 32; bit++) {
        set_result[bit] = sceKernelChangeCurrentThreadAttr(0, 1u << bit);
        attr_after_set[bit] = attr_of_self();
        clear_result[bit] = sceKernelChangeCurrentThreadAttr(1u << bit, 0);
        attr_after_clear[bit] = attr_of_self();
    }
    return 0;
}

int main(void) {
    const SceUID thread = test_start_thread(changing_thread);
    test_join_thread(thread);
    for (int bit = 0; bit < 32; bit++) {
        PRINTF(set_result[bit], "set 0x%08X", 1u << bit);
        PRINTF(attr_after_set[bit], "the attributes after setting 0x%08X", 1u << bit);
        PRINTF(clear_result[bit], "clear 0x%08X", 1u << bit);
        PRINTF(attr_after_clear[bit], "the attributes after clearing 0x%08X", 1u << bit);
    }

    test_exit();
    return 0;
}
