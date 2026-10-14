// What r1, r2, r3 and r12 hold after a call to a function the thread manager exports as a syscall,
// when they held marker values before it: sceKernelDelayThread with one argument and
// sceKernelPollSema with two. The calling convention lets a called function change them, so the
// test prints what each one holds afterwards, and whether it still holds its marker.
//
// On a PS Vita TV on firmware 3.65 every one of them that isn't the result reads 0xDEADBEEF after
// the syscall.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

static SceUID sema;

// The imports themselves, so nothing compiled here runs between the call and the syscall
#define DELAY ((int (*)(int, int))(void *)sceKernelDelayThread)
#define POLL ((int (*)(int, int))(void *)sceKernelPollSema)

typedef struct Call {
    int (*fn)(int, int);
    int a0, a1, one_argument;
    unsigned int out[5];
} Call;

// Calls fn(a0, a1) with r2, r3 and r12, and r1 if fn takes one argument, set to markers, and keeps
// r0 to r3 and r12 as fn left them
static void call_marked(Call *call) {
    __asm__ volatile(
        "ldr r0, [%[c], #4]\n\t"
        "ldr r1, [%[c], #8]\n\t"
        "ldr r3, [%[c], #12]\n\t"
        "cmp r3, #0\n\t"
        "beq 1f\n\t"
        "movw r1, #0x1111\n\t"
        "movt r1, #0x1111\n\t"
        "1:\n\t"
        "movw r2, #0x2222\n\t"
        "movt r2, #0x2222\n\t"
        "movw r3, #0x3333\n\t"
        "movt r3, #0x3333\n\t"
        "movw r12, #0xCCCC\n\t"
        "movt r12, #0xCCCC\n\t"
        "ldr lr, [%[c]]\n\t"
        "blx lr\n\t"
        "str r0, [%[c], #16]\n\t"
        "str r1, [%[c], #20]\n\t"
        "str r2, [%[c], #24]\n\t"
        "str r3, [%[c], #28]\n\t"
        "str r12, [%[c], #32]\n\t"
        :
        : [c] "r"(call)
        : "r0", "r1", "r2", "r3", "r12", "lr", "memory", "cc");
}

static void report(const char *name, const unsigned int out[5], int one_argument) {
    static const unsigned int markers[5] = { 0, 0x11111111, 0x22222222, 0x33333333, 0xCCCCCCCC };
    static const char *const regs[5] = { "r0", "r1", "r2", "r3", "r12" };
    PRINTF(out[0], "%s: r0, the result", name);
    for (int i = one_argument ? 1 : 2; i < 5; i++) {
        PRINTF(out[i], "%s: %s after it", name, regs[i]);
        PRINTF(out[i] == markers[i], "%s: %s still holds its marker", name, regs[i]);
    }
}

int main(void) {
    Call call = { DELAY, 1, 0, 1, { 0 } };
    call_marked(&call);
    report("sceKernelDelayThread(1)", call.out, 1);
    sema = sceKernelCreateSema("sema", 0, 1, 1, NULL);
    call = (Call){ POLL, sema, 1, 0, { 0 } };
    call_marked(&call);
    report("sceKernelPollSema with a count there", call.out, 0);
    call = (Call){ POLL, sema, 1, 0, { 0 } };
    call_marked(&call);
    report("sceKernelPollSema with no count there", call.out, 0);
    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}
