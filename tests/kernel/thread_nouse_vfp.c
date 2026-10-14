// Threads with the attribute 0x01000000, which the Vita Development Wiki calls
// SCE_KERNEL_THREAD_ATTR_NOUSE_VFP: whether a waiting thread keeps its rounding mode and the VFP
// registers d8, which a called function must keep, and d16, which it may change, while another
// thread on its core changes all three, with the attribute on the waiting thread, on the other
// one, or set by the waiting thread on itself.
#include <psp2/kernel/threadmgr.h>

#include "registers.h"
#include "test.h"
#include "threadmgr_extra.h"

static SceUID sema;
static volatile int set_on_self, set_result, after_rmode;
static const unsigned long long waiter_regs[2] = { 0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL };
static const unsigned long long changer_regs[2] = { 0x0102030405060708ULL, 0x1020304050607080ULL };
static unsigned long long after_regs[2];

static int wait_sema(int id, int count, void *timeout) {
    return sceKernelWaitSema(id, count, timeout);
}

static int signal_sema(int id, int count, void *unused) {
    (void)unused;
    return sceKernelSignalSema(id, count);
}

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    if (set_on_self)
        set_result = sceKernelChangeCurrentThreadAttr(0, THREAD_ATTR_NOUSE_VFP);
    set_rmode(RMODE_PLUS);
    SceUInt timeout = TEST_WAIT_US;
    call_with_d8_d16(wait_sema, sema, 1, &timeout, waiter_regs, after_regs);
    after_rmode = get_rmode();
    return 0;
}

static int changer(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    set_rmode(RMODE_MINUS);
    call_with_d8_d16(signal_sema, sema, 1, NULL, changer_regs, NULL);
    return 0;
}

static void run(const char *name, SceUInt waiter_attr, SceUInt changer_attr, int self) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    set_on_self = self;
    set_result = after_rmode = 0xDEAD;
    after_regs[0] = after_regs[1] = 0xDEAD;
    // Both on core 1, so the changer runs on the core the waiter waits on
    const SceUID w = sceKernelCreateThread("waiter", waiter, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, waiter_attr, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    sceKernelStartThread(w, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(w, WAIT_TYPE_SEMA, sema));
    const SceUID c = sceKernelCreateThread("changer", changer, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, changer_attr, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    sceKernelStartThread(c, 0, NULL);
    test_join_thread(c);
    test_join_thread(w);
    sceKernelDeleteSema(sema);
    if (self)
        PRINTF(set_result, "%s: set the attribute", name);
    PRINTF(after_rmode, "%s: the waiter's rounding mode after", name);
    static const char *regs[2] = { "d8", "d16" };
    for (int i = 0; i < 2; i++) {
        PRINTF(after_regs[i] == waiter_regs[i], "%s: the waiter's %s is its own after", name, regs[i]);
        PRINTF(after_regs[i] == changer_regs[i], "%s: the waiter's %s is the other thread's after", name, regs[i]);
        PRINTF(after_regs[i] == 0, "%s: the waiter's %s is 0 after", name, regs[i]);
    }
}

int main(void) {
    run("neither has it", 0, 0, 0);
    run("the waiter has it", THREAD_ATTR_NOUSE_VFP, 0, 0);
    run("the other thread has it", 0, THREAD_ATTR_NOUSE_VFP, 0);
    run("both have it", THREAD_ATTR_NOUSE_VFP, THREAD_ATTR_NOUSE_VFP, 0);
    run("the waiter sets it on itself", 0, 0, 1);
    test_exit();
    return 0;
}
