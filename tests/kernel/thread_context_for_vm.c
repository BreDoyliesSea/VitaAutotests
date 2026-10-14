// sceKernelGetThreadContextForVM, which reads the registers of another thread: whether the caller
// or the target needs the attribute 0x04000000, which the Vita Development Wiki calls
// SCE_KERNEL_THREAD_ATTR_VM, whether the target must be suspended with
// sceKernelSuspendThreadForVM, its errors, and what it reports for a suspended waiting thread: its
// mode, its stack pointer, TPIDRURW, the FPSCR control bits, d8 and d16. Both threads run on core 1, and
// the caller uses VFP before it reads, so the kernel has saved the target's VFP registers by then.
// Values are printed as comparisons.
#include <psp2/kernel/threadmgr.h>

#include "registers.h"
#include "test.h"
#include "threadmgr_extra.h"

#define VM 0x04000000
static const unsigned long long target_regs[2] = { 0x1122334455667788ULL, 0x0123456789ABCDEFULL };

static SceUID sema, target;
static SceUIntPtr target_stack_low, target_stack_high;

typedef struct Read {
    int unsuspended, suspend, result, resume;
    unsigned int mode, thumb, sp_on_stack, tpidrurw, fpscr, d8, d16;
} Read;

static Read reads[4];
static volatile int errors[8], self_result;

static int wait_sema(int id, int count, void *timeout) {
    return sceKernelWaitSema(id, count, timeout);
}

static int target_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    set_tpidrurw(0x5555);
    set_rmode(RMODE_PLUS);
    SceUInt timeout = TEST_WAIT_US;
    call_with_d8_d16(wait_sema, sema, 1, &timeout, target_regs, NULL);
    return 0;
}

static int read_into(ThreadCpuRegisters *cpu, ThreadVfpRegisters *vfp) {
    cpu->size = sizeof(*cpu);
    vfp->size = sizeof(*vfp);
    return sceKernelGetThreadContextForVM(target, cpu, vfp);
}

static void errors_on_suspended_target(void) {
    ThreadCpuRegisters cpu = { .size = sizeof(cpu) };
    ThreadVfpRegisters vfp = { .size = sizeof(vfp) };
    errors[0] = sceKernelGetThreadContextForVM(0x12345, &cpu, &vfp);
    errors[1] = sceKernelGetThreadContextForVM(target, NULL, &vfp);
    errors[2] = sceKernelGetThreadContextForVM(target, &cpu, NULL);
    errors[3] = sceKernelGetThreadContextForVM(target, NULL, NULL);
    cpu.size = 0;
    errors[4] = sceKernelGetThreadContextForVM(target, &cpu, &vfp);
    cpu.size = sizeof(cpu) + 4;
    errors[5] = sceKernelGetThreadContextForVM(target, &cpu, &vfp);
    cpu.size = sizeof(cpu);
    vfp.size = sizeof(vfp) - 4;
    errors[6] = sceKernelGetThreadContextForVM(target, &cpu, &vfp);
    vfp.size = sizeof(vfp);
    errors[7] = sceKernelGetThreadContextForVM(0, &cpu, &vfp);
    self_result = sceKernelGetThreadContextForVM(sceKernelGetThreadId(), &cpu, &vfp);
}

static volatile int which;

static int caller_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    Read *r = &reads[which];
    ThreadCpuRegisters cpu;
    ThreadVfpRegisters vfp;
    r->unsuspended = read_into(&cpu, &vfp);
    r->suspend = sceKernelSuspendThreadForVM(target);
    // VFP use on the target's core, so its VFP registers are saved
    set_rmode(RMODE_ZERO);
    if (which == 3)
        errors_on_suspended_target();
    sceClibMemset(&cpu, 0, sizeof(cpu));
    sceClibMemset(&vfp, 0, sizeof(vfp));
    r->result = read_into(&cpu, &vfp);
    r->mode = cpu.cpsr & 0x1F;
    r->thumb = (cpu.cpsr >> 5) & 1;
    r->sp_on_stack = cpu.reg[13] > target_stack_low && cpu.reg[13] <= target_stack_high;
    r->tpidrurw = cpu.tpidrurw;
    r->fpscr = vfp.fpscr & FPSCR_CONTROL;
    r->d8 = vfp.reg[16] == (SceUInt32)target_regs[0] && vfp.reg[17] == (SceUInt32)(target_regs[0] >> 32);
    r->d16 = vfp.reg[32] == (SceUInt32)target_regs[1] && vfp.reg[33] == (SceUInt32)(target_regs[1] >> 32);
    r->resume = sceKernelResumeThreadForVM(target);
    return 0;
}

static void run(int index, SceUInt target_attr, SceUInt caller_attr) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    target = sceKernelCreateThread("target", target_entry, 0x10000100, 0x4000, target_attr, CPU_MASK_USER_1, NULL);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(target, &info);
    target_stack_low = (SceUIntPtr)info.stack;
    target_stack_high = target_stack_low + info.stackSize;
    sceKernelStartThread(target, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(target, WAIT_TYPE_SEMA, sema));
    which = index;
    const SceUID caller = sceKernelCreateThread("caller", caller_entry, 0x10000100, 0x4000, caller_attr, CPU_MASK_USER_1, NULL);
    sceKernelStartThread(caller, 0, NULL);
    test_join_thread(caller);
    sceKernelSignalSema(sema, 1);
    test_join_thread(target);
    sceKernelDeleteSema(sema);
}

int main(void) {
    static const char *names[4] = { "neither has the attribute", "the target has it", "the caller has it", "both have it" };
    run(0, 0, 0);
    run(1, VM, 0);
    run(2, 0, VM);
    run(3, VM, VM);
    for (int i = 0; i < 4; i++) {
        const Read *r = &reads[i];
        PRINTF(r->unsuspended, "%s: read the waiting thread", names[i]);
        PRINTF(r->suspend, "%s: suspend it", names[i]);
        PRINTF(r->result, "%s: read it suspended", names[i]);
        if (r->result == 0) {
            PRINTF(r->mode, "%s: its mode", names[i]);
            PRINTF(r->thumb, "%s: its Thumb bit", names[i]);
            PRINTF(r->sp_on_stack, "%s: its stack pointer is on its stack", names[i]);
            PRINTF(r->tpidrurw, "%s: its TPIDRURW", names[i]);
            PRINTF(r->fpscr, "%s: its FPSCR control bits", names[i]);
            PRINTF(r->d8, "%s: its d8 is the value it set", names[i]);
            PRINTF(r->d16, "%s: its d16 is the value it set", names[i]);
        }
        PRINTF(r->resume, "%s: resume it", names[i]);
    }
    PRINT("suspended target: an unknown UID", errors[0]);
    PRINT("suspended target: no CPU registers", errors[1]);
    PRINT("suspended target: no VFP registers", errors[2]);
    PRINT("suspended target: neither", errors[3]);
    PRINT("suspended target: a CPU size of 0", errors[4]);
    PRINT("suspended target: a CPU size 4 too big", errors[5]);
    PRINT("suspended target: a VFP size 4 too small", errors[6]);
    PRINT("suspended target: UID 0", errors[7]);
    PRINT("suspended target: the calling thread itself", self_result);
    test_exit();
    return 0;
}
