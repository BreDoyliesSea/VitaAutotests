// The CPSR a new thread reads with MRS, for a thread whose entry is ARM code. The entry reads it
// first thing, and again after a compare of two equal values, which sets the Z and C flags. A
// thread whose entry is Thumb code is left out for now, see thumb_entry below.
//
// On a PS Vita TV on firmware 3.65 it reads 0x10, user mode with no flags, and 0x60000010 after the
// compare.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

static volatile unsigned int first, after_compare;

// Called by the entries with the two values they read
int record_cpsr(unsigned int at_start, unsigned int after);
int record_cpsr(unsigned int at_start, unsigned int after) {
    first = at_start;
    after_compare = after;
    return 0;
}

// Reads the CPSR, compares a register with itself and reads it again, then records both
__attribute__((naked, target("arm"))) static int arm_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    __asm__ volatile(
        "mrs r0, apsr\n\t"
        "cmp r0, r0\n\t"
        "mrs r1, apsr\n\t"
        "push {r4, lr}\n\t"
        "bl record_cpsr\n\t"
        "pop {r4, pc}\n\t");
}

// The Thumb entry is left out for now. On a PS Vita TV on firmware 3.65 it reads 0x10 first thing
// and 0x60000010 after the compare, the same as the ARM entry, since MRS never shows the T bit.
// The ARMv7-A Architecture Reference Manual, "Accessing the execution state bits": in the CPSR,
// unless the processor is in Debug state, the execution state bits, other than the E bit, are RAZ
// when read by an MRS instruction. Its MRS pseudocode masks them out:
//     R[d] = CPSR AND '11111000 11111111 00000011 11011111'
// Dynarmic, the CPU Vita3K uses, doesn't mask them. Its arm_MRS and thumb32_MRS_reg do
//     ir.SetRegister(d, ir.GetCpsr())
// and A32JitState::Cpsr() builds that value with the T bit and the IT state of the running code:
//     cpsr |= mcl::bit::get_bit<0>(upper_location_descriptor) ? 1 << 5 : 0
//     cpsr |= static_cast<u32>(upper_location_descriptor & 0b11111100'00000000)
// So Thumb code reads the T bit, 0x20, there. No game is known to read the T bit with MRS. With
// the mask in Dynarmic, ir.And(ir.GetCpsr(), ir.Imm32(0xF8FF03DF)), this case passes.
#if 0
// The same in Thumb code
__attribute__((naked, target("thumb"))) static int thumb_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    __asm__ volatile(
        "mrs r0, apsr\n\t"
        "cmp r0, r0\n\t"
        "mrs r1, apsr\n\t"
        "push {r4, lr}\n\t"
        "bl record_cpsr\n\t"
        "pop {r4, pc}\n\t");
}
#endif

static void run_case(const char *name, SceKernelThreadEntry entry) {
    first = after_compare = 0xDEAD;
    const SceUID thread = sceKernelCreateThread("cpsr", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    const int start = sceKernelStartThread(thread, 0, NULL);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitThreadEnd(thread, NULL, &timeout);
    sceKernelDeleteThread(thread);
    PRINTF(start, "%s: start", name);
    PRINTF(first, "%s: the cpsr first thing", name);
    PRINTF(after_compare, "%s: the cpsr after a compare of equal values", name);
}

int main(void) {
    run_case("an ARM entry", arm_entry);
    // Left out for now, see thumb_entry
    // run_case("a Thumb entry", thumb_entry);
    test_exit();
    return 0;
}
