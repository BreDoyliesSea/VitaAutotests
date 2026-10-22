// The registers a thread starts with for each argument size. The argument is copied to the top of
// the thread's stack, rounded up to a multiple of 16 bytes, so the output shows r0 (the size), r1
// (the copy) and sp for sizes on both sides of that rounding, and whether the copy holds the bytes
// that were sent. An argument of size 0 or with a NULL pointer is not copied, and the thread starts
// with r0 and r1 at 0 and sp at the stack top. Registers pointing into the thread's stack are
// printed as their distance below the stack top. Each case starts a new thread.
//
// On a PS Vita TV on firmware 3.65 the copy and sp sit at the stack top minus the size rounded up
// to 16, r0 is the size and r1 points at the copy. With a size of 0 or a NULL pointer, r0 and r1
// are 0 and sp is the stack top.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

typedef struct Seen {
    unsigned int r0, r1, sp;
    unsigned int stack_low, stack_high;
    int ran, copy_matches;
} Seen;

static Seen seen;

// The bytes sent as the argument, each different from the next
static unsigned char known[32] = {
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
    0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
};

// Called by the entry with the registers it pushed
void record_arg_start(const unsigned int *pushed);
void record_arg_start(const unsigned int *pushed) {
    Seen *s = &seen;
    s->r0 = pushed[0];
    s->r1 = pushed[1];
    s->sp = (unsigned int)pushed + 14 * 4;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
    s->stack_low = (unsigned int)info.stack;
    s->stack_high = s->stack_low + info.stackSize;
    // The size is only used once it is known to be small and the copy to be inside the stack
    s->copy_matches = s->r0 <= sizeof(known) && s->r1 >= s->stack_low && s->r1 + s->r0 <= s->stack_high &&
                      sceClibMemcmp((const void *)s->r1, known, s->r0) == 0;
    s->ran = 1;
}

// Pushes every register before anything changes one, records them, and returns 0
__attribute__((naked)) static int start_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    __asm__ volatile(
        "push {r0-r12, lr}\n\t"
        "mov r0, sp\n\t"
        "bl record_arg_start\n\t"
        "pop {r0-r12, lr}\n\t"
        "movs r0, #0\n\t"
        "bx lr\n\t");
}

static void print_register(const char *name, const char *reg, unsigned int value) {
    if (value >= seen.stack_low && value <= seen.stack_high)
        PRINTF(seen.stack_high - value, "%s: %s, below the stack top by", name, reg);
    else
        PRINTF(value, "%s: %s", name, reg);
}

// Starts a new thread with the argument, waits for its end and prints what it started with
static void run_case(const char *name, SceSize size, void *arg) {
    seen = (Seen){ 0 };
    const SceUID thread = sceKernelCreateThread("start args", start_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    PRINTF(sceKernelStartThread(thread, size, arg), "%s: start", name);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitThreadEnd(thread, NULL, &timeout);
    sceKernelDeleteThread(thread);

    PRINTF(seen.ran, "%s: the thread ran", name);
    print_register(name, "r0", seen.r0);
    print_register(name, "r1", seen.r1);
    print_register(name, "sp", seen.sp);
    if (arg && size)
        PRINTF(seen.copy_matches, "%s: the copy holds the bytes sent", name);
}

int main(void) {
    run_case("size 1", 1, known);
    run_case("size 15", 15, known);
    run_case("size 16", 16, known);
    run_case("size 17", 17, known);
    run_case("size 32", 32, known);

    // No copy is made for these, the last one is the plain case to compare them with
    run_case("size 0 with a pointer", 0, known);
    run_case("size 4 with a NULL pointer", 4, NULL);
    run_case("size 0 with a NULL pointer", 0, NULL);

    test_exit();
    return 0;
}
