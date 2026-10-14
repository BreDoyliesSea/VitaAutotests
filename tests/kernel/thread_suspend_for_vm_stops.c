// Whether sceKernelSuspendThreadForVM returns only once the thread has stopped, for a thread on
// another core that runs its own code without syscalls, and for one that calls a syscall that
// doesn't wait over and over. Each thread counts its loops. Right after the suspend returns the
// test reads the count and the thread's registers, waits 20 ms and reads both again, then prints
// whether either changed, and whether the thread runs again after the resume.
//
// On a PS Vita TV on firmware 3.65 neither changes in both cases: the suspend only returns once the
// thread has stopped.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID empty_sema;
static volatile int stop;
static volatile unsigned int counter;

static int spin_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    while (!stop)
        counter++;
    return 0;
}

static int poll_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    while (!stop) {
        sceKernelPollSema(empty_sema, 1);
        counter++;
    }
    return 0;
}

static int read_cpu(SceUID thread, ThreadCpuRegisters *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->size = sizeof(*cpu);
    return sceKernelGetThreadContextForVM(thread, cpu, NULL);
}

static void run_case(const char *name, SceKernelThreadEntry entry) {
    stop = 0;
    counter = 0;
    const SceUID thread = sceKernelCreateThread("target", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, THREAD_ATTR_VM, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(counter > 0);

    // Printing waits, so everything is read before it
    const int suspended = sceKernelSuspendThreadForVM(thread);
    const unsigned int first = counter;
    ThreadCpuRegisters before, after;
    const int read_before = read_cpu(thread, &before);
    sceKernelDelayThread(20000);
    const unsigned int second = counter;
    const int read_after = read_cpu(thread, &after);
    PRINTF(suspended, "%s: suspend it", name);
    PRINTF(read_before, "%s: read its registers", name);
    PRINTF(read_after, "%s: read them again 20 ms later", name);
    PRINTF(second == first, "%s: the count stays as it was right after the suspend", name);
    PRINTF(memcmp(before.reg, after.reg, sizeof(before.reg)) == 0 && before.cpsr == after.cpsr, "%s: both reads show the same registers", name);

    PRINTF(sceKernelResumeThreadForVM(thread), "%s: resume it", name);
    TEST_WAIT_FOR(counter != second);
    PRINTF(counter != second, "%s: it runs again after the resume", name);
    stop = 1;
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    sceKernelDeleteThread(thread);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    empty_sema = sceKernelCreateSema("empty", 0, 0, 1, NULL);
    run_case("running its own code", spin_entry);
    run_case("calling a syscall that doesn't wait", poll_entry);
    sceKernelDeleteSema(empty_sema);
    test_exit();
    return 0;
}
