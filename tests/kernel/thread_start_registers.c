// The core registers a thread starts with, r0 to r12, sp, lr and the CPSR: for a first start with
// an argument, a restart without one, a restart from plain C code, a new thread, and a thread
// started by another thread. The starting thread holds markers in r4 to r11 when it calls
// sceKernelStartThread, so the output shows whether any of them reach the new thread. A register
// pointing into the thread's stack is printed as its distance below the stack top, and lr only as
// comparisons.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

typedef struct Seen {
    unsigned int r[13];
    unsigned int sp, lr, cpsr;
    unsigned int stack_low, stack_high;
} Seen;

#define RUNS 5
static Seen seen[RUNS];
static volatile int run;

// Called by the entry with the registers it pushed and the CPSR it read
void record_start(const unsigned int *pushed, unsigned int cpsr);
void record_start(const unsigned int *pushed, unsigned int cpsr) {
    Seen *s = &seen[run];
    for (int i = 0; i < 13; i++)
        s->r[i] = pushed[i];
    s->lr = pushed[13];
    s->sp = (unsigned int)pushed + 14 * 4;
    s->cpsr = cpsr;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
    s->stack_low = (unsigned int)info.stack;
    s->stack_high = s->stack_low + info.stackSize;
}

// Pushes every register before anything changes one, records them, and returns 0
__attribute__((naked)) static int start_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    __asm__ volatile(
        "push {r0-r12, lr}\n\t"
        "mov r0, sp\n\t"
        "mrs r1, apsr\n\t"
        "bl record_start\n\t"
        "pop {r0-r12, lr}\n\t"
        "movs r0, #0\n\t"
        "bx lr\n\t");
}

// Calls fn(a0, a1, a2) with r4 to r11 set to 0x44444444 to 0xBBBBBBBB
__attribute__((naked)) static int call_marked(int a0, int a1, int a2, int (*fn)(int, int, int)) {
    (void)a0;
    (void)a1;
    (void)a2;
    (void)fn;
    __asm__ volatile(
        "push {r3-r11, lr}\n\t"
        "movw r4, #0x4444\n\tmovt r4, #0x4444\n\t"
        "movw r5, #0x5555\n\tmovt r5, #0x5555\n\t"
        "movw r6, #0x6666\n\tmovt r6, #0x6666\n\t"
        "movw r7, #0x7777\n\tmovt r7, #0x7777\n\t"
        "movw r8, #0x8888\n\tmovt r8, #0x8888\n\t"
        "movw r9, #0x9999\n\tmovt r9, #0x9999\n\t"
        "movw r10, #0xAAAA\n\tmovt r10, #0xAAAA\n\t"
        "movw r11, #0xBBBB\n\tmovt r11, #0xBBBB\n\t"
        "blx r3\n\t"
        "pop {r3-r11, pc}\n\t");
}

// The import itself, so the markers reach the syscall
#define START ((int (*)(int, int, int))(void *)sceKernelStartThread)

static void print_register(const char *name, const Seen *s, const char *reg, unsigned int value) {
    if (value >= s->stack_low && value <= s->stack_high)
        PRINTF(s->stack_high - value, "%s: %s, below the stack top by", name, reg);
    else
        PRINTF(value, "%s: %s", name, reg);
}

static void report(const char *name, int i) {
    const Seen *s = &seen[i];
    char reg[8];
    for (int r = 0; r < 13; r++) {
        sceClibSnprintf(reg, sizeof(reg), "r%d", r);
        print_register(name, s, reg, s->r[r]);
    }
    print_register(name, s, "sp", s->sp);
    PRINTF(s->lr & 1, "%s: lr is Thumb", name);
    PRINTF(s->lr == seen[0].lr, "%s: lr is the first start's", name);
    PRINTF(s->cpsr, "%s: cpsr", name);
}

static int starter_thread(SceSize args, void *argp) {
    (void)args;
    const SceUID thread = *(const SceUID *)argp;
    int arg = 0x1234;
    return call_marked(thread, sizeof(arg), (int)&arg, START);
}

int main(void) {
    int arg = 0x1234;
    const SceUID thread = sceKernelCreateThread("start regs", start_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);

    run = 0;
    PRINT("start with an argument", call_marked(thread, sizeof(arg), (int)&arg, START));
    sceKernelWaitThreadEnd(thread, NULL, NULL);

    run = 1;
    PRINT("restart without an argument", call_marked(thread, 0, 0, START));
    sceKernelWaitThreadEnd(thread, NULL, NULL);

    run = 2;
    PRINT("restart from C", sceKernelStartThread(thread, 0, NULL));
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    sceKernelDeleteThread(thread);

    run = 3;
    const SceUID fresh = sceKernelCreateThread("start regs", start_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    PRINT("start a new thread from C", sceKernelStartThread(fresh, 0, NULL));
    sceKernelWaitThreadEnd(fresh, NULL, NULL);
    sceKernelDeleteThread(fresh);

    run = 4;
    SceUID other = sceKernelCreateThread("start regs", start_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    const SceUID starter = sceKernelCreateThread("starter", starter_thread, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    sceKernelStartThread(starter, sizeof(other), &other);
    sceKernelWaitThreadEnd(starter, NULL, NULL);
    sceKernelWaitThreadEnd(other, NULL, NULL);
    sceKernelDeleteThread(starter);
    sceKernelDeleteThread(other);

    report("first start", 0);
    report("restart", 1);
    report("restart from C", 2);
    report("new thread from C", 3);
    report("from another thread", 4);
    test_exit();
    return 0;
}
