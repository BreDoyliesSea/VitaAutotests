// Which VFP registers sceKernelGetThreadContextForVM reports as the thread left them: for a thread
// that waits in a syscall, read right after the suspend and again after the reading thread used
// VFP on the same core, and for a thread suspended while it runs its own code. Each thread sets d0
// to d31 to values of its own and a rounding mode first. For each read the test prints which d
// registers have the thread's value, which read the other value they all share and what that is,
// and the FPSCR control bits.
//
// On a PS Vita TV on firmware 3.65, a thread waiting in a syscall shows only d8 to d15 as it left
// them, the registers the ARM calling convention has a called function preserve. d0 to d7 and d16
// to d31, which a called function may change, all read 0x7FF8DEAD7F80DEAD, a NaN as a double whose
// halves 0x7F80DEAD and 0x7FF8DEAD are NaNs as floats too. Its FPSCR reads 0x03000000 instead of
// its own. Both reads show the same. A thread suspended while it runs its own code shows all of d0
// to d31 and its own FPSCR.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "registers.h"
#include "test.h"
#include "threadmgr_extra.h"

static SceUID sema, target;
static unsigned long long values[32];
static volatile int spinning, stop_spin;

static int wait_sema(int id, int count, void *timeout) {
    return sceKernelWaitSema(id, count, timeout);
}

// Loads d0 to d31 from values, then calls fn
static int call_with_all_d(int (*fn)(int, int, void *), int a0, int a1, void *a2) {
    register int r0 __asm__("r0") = a0;
    register int r1 __asm__("r1") = a1;
    register void *r2 __asm__("r2") = a2;
    __asm__ volatile(
        "vldmia %[in]!, {d0-d15}\n\t"
        "vldmia %[in], {d16-d31}\n\t"
        "blx %[fn]\n\t"
        : "+r"(r0), "+r"(r1), "+r"(r2)
        : [in] "r"(values), [fn] "r"(fn)
        : "r3", "r12", "lr", "memory", "cc", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9",
          "d10", "d11", "d12", "d13", "d14", "d15", "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
          "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31");
    return r0;
}

static int waiting_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    set_rmode(RMODE_PLUS);
    SceUInt timeout = TEST_WAIT_US;
    call_with_all_d(wait_sema, sema, 1, &timeout);
    return 0;
}

// Loads d0 to d31 from values and spins without syscalls or VFP until told to stop
static int spinning_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    set_rmode(RMODE_MINUS);
    __asm__ volatile(
        "vldmia %[in]!, {d0-d15}\n\t"
        "vldmia %[in], {d16-d31}\n\t"
        "mov r1, #1\n\t"
        "str r1, [%[spinning]]\n\t"
        "1:\n\t"
        "ldr r1, [%[stop]]\n\t"
        "cmp r1, #0\n\t"
        "beq 1b\n\t"
        :
        : [in] "r"(values), [spinning] "r"(&spinning), [stop] "r"(&stop_spin)
        : "r1", "memory", "cc", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9", "d10", "d11",
          "d12", "d13", "d14", "d15", "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", "d24", "d25",
          "d26", "d27", "d28", "d29", "d30", "d31");
    return 0;
}

static void report(const char *when) {
    ThreadCpuRegisters cpu = { .size = sizeof(cpu) };
    ThreadVfpRegisters vfp = { .size = sizeof(vfp) };
    PRINTF(sceKernelGetThreadContextForVM(target, &cpu, &vfp), "%s: the read", when);
    PRINTF(vfp.fpscr & FPSCR_CONTROL, "%s: the FPSCR control bits", when);
    // The value the first register without the thread's value has, and the registers that share it
    unsigned long long other = 0;
    int kept = 0, shared = 0, others = 0;
    for (int d = 0; d < 32; d++) {
        const unsigned long long got = (unsigned long long)vfp.reg[2 * d + 1] << 32 | vfp.reg[2 * d];
        if (got == values[d]) {
            kept |= 1 << d;
        } else if (!others || got == other) {
            other = got;
            shared |= 1 << d;
            others++;
        }
    }
    PRINTF(kept, "%s: the d registers with the thread's value, one bit each", when);
    PRINTF(shared, "%s: the d registers that share the other value", when);
    PRINTF((unsigned int)(other >> 32), "%s: that value's high word", when);
    PRINTF((unsigned int)other, "%s: that value's low word", when);
}

static SceUID start(SceKernelThreadEntry entry) {
    const SceUID thread = sceKernelCreateThread("target", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, THREAD_ATTR_VM, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    sceKernelStartThread(thread, 0, NULL);
    return thread;
}

int main(void) {
    for (int d = 0; d < 32; d++)
        values[d] = (unsigned long long)(0xC1000000U + (unsigned int)d) << 32 | (0xC2000000U + (unsigned int)d);

    // A thread that waits in a syscall, read from its own core
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    target = start(waiting_entry);
    TEST_WAIT_FOR(test_in_wait(target, WAIT_TYPE_SEMA, sema));
    PRINT("waiting: suspend it", sceKernelSuspendThreadForVM(target));
    report("waiting, right after the suspend");
    // VFP use on the target's core, so the kernel saved its VFP registers
    volatile double x = 1.5, y = 2.25;
    volatile double z = x * y;
    (void)z;
    report("waiting, after the reader used VFP");
    PRINT("waiting: resume it", sceKernelResumeThreadForVM(target));
    sceKernelSignalSema(sema, 1);
    test_join_thread(target);

    // A thread suspended while it runs its own code on core 1, read from core 2
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    sceKernelDelayThread(1000);
    spinning = stop_spin = 0;
    target = start(spinning_entry);
    TEST_WAIT_FOR(spinning);
    PRINT("running: suspend it", sceKernelSuspendThreadForVM(target));
    report("running, suspended");
    PRINT("running: resume it", sceKernelResumeThreadForVM(target));
    stop_spin = 1;
    test_join_thread(target);

    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}
