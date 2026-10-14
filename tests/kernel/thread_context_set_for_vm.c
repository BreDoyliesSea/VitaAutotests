// sceKernelSetThreadContextForVM, which writes the registers of a thread that
// sceKernelSuspendThreadForVM suspended: its errors, and what writing only the VFP registers of a
// thread waiting in a syscall does. The thread sets d8 and d16 before it waits and keeps what they
// hold after the wait. The test writes new values into both, and prints whether the next read
// shows them and whether the thread has them once it runs again.
//
// On a PS Vita TV on firmware 3.65 the write works for a suspended thread and the next read shows
// it. Once the thread runs again it has the new d8, which a syscall keeps, but not the new d16,
// which it doesn't.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "registers.h"
#include "test.h"
#include "threadmgr_extra.h"

int sceKernelSetThreadContextForVM(SceUID thid, const ThreadCpuRegisters *cpu, const ThreadVfpRegisters *vfp);

static SceUID sema, target;
static const unsigned long long set_values[2] = { 0x1111111122222222ULL, 0x3333333344444444ULL };
static const unsigned long long new_values[2] = { 0x5555555566666666ULL, 0x7777777788888888ULL };
static unsigned long long after[2];

static int wait_sema(int id, int count, void *timeout) {
    return sceKernelWaitSema(id, count, timeout);
}

static int target_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    call_with_d8_d16(wait_sema, sema, 1, &timeout, set_values, after);
    return 0;
}

static unsigned long long d_of(const ThreadVfpRegisters *vfp, int d) {
    return (unsigned long long)vfp->reg[2 * d + 1] << 32 | vfp->reg[2 * d];
}

static void set_d(ThreadVfpRegisters *vfp, int d, unsigned long long value) {
    vfp->reg[2 * d] = (SceUInt32)value;
    vfp->reg[2 * d + 1] = (SceUInt32)(value >> 32);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    target = sceKernelCreateThread("target", target_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, THREAD_ATTR_VM, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    sceKernelStartThread(target, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(target, WAIT_TYPE_SEMA, sema));

    ThreadCpuRegisters cpu = { .size = sizeof(cpu) };
    ThreadVfpRegisters vfp = { .size = sizeof(vfp) };
    PRINT("a thread that isn't suspended", sceKernelSetThreadContextForVM(target, NULL, &vfp));
    PRINT("suspend it", sceKernelSuspendThreadForVM(target));
    PRINT("an unknown thread", sceKernelSetThreadContextForVM(0x12345, NULL, &vfp));
    PRINT("thread ID 0", sceKernelSetThreadContextForVM(0, NULL, &vfp));
    PRINT("the calling thread", sceKernelSetThreadContextForVM(sceKernelGetThreadId(), NULL, &vfp));
    PRINT("no registers at all", sceKernelSetThreadContextForVM(target, NULL, NULL));
    vfp.size = sizeof(vfp) - 4;
    PRINT("VFP registers 4 bytes too small", sceKernelSetThreadContextForVM(target, NULL, &vfp));
    vfp.size = sizeof(vfp) + 4;
    PRINT("VFP registers 4 bytes too big", sceKernelSetThreadContextForVM(target, NULL, &vfp));
    vfp.size = sizeof(vfp);
    cpu.size = sizeof(cpu) - 4;
    PRINT("CPU registers 4 bytes too small, with VFP registers", sceKernelSetThreadContextForVM(target, &cpu, &vfp));
    cpu.size = sizeof(cpu);

    PRINT("read the context", sceKernelGetThreadContextForVM(target, &cpu, &vfp));
    PRINT("d8 has what the thread set", d_of(&vfp, 8) == set_values[0]);
    set_d(&vfp, 8, new_values[0]);
    set_d(&vfp, 16, new_values[1]);
    PRINT("write new d8 and d16", sceKernelSetThreadContextForVM(target, NULL, &vfp));
    ThreadVfpRegisters back = { .size = sizeof(back) };
    PRINT("read it again", sceKernelGetThreadContextForVM(target, &cpu, &back));
    PRINT("the read shows the new d8", d_of(&back, 8) == new_values[0]);
    PRINT("the read shows the new d16", d_of(&back, 16) == new_values[1]);
    PRINT("the read shows the d8 the thread set", d_of(&back, 8) == set_values[0]);

    PRINT("resume it", sceKernelResumeThreadForVM(target));
    sceKernelSignalSema(sema, 1);
    test_join_thread(target);
    PRINT("after the wait, the thread has the new d8", after[0] == new_values[0]);
    PRINT("after the wait, the thread has the d8 it set", after[0] == set_values[0]);
    PRINT("after the wait, the thread has the new d16", after[1] == new_values[1]);
    PRINT("after the wait, the thread has the d16 it set", after[1] == set_values[1]);

    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}
