// What every sceKernelAtomic call returns and leaves in memory, at 8, 16, 32 and 64 bits. Each
// operation starts from a known value and prints the call's result and the value after it, and
// each line names the start value and the arguments. The cases show whether a call returns the
// old or the new value, what CompareAndSet returns when it swaps and when it doesn't, how an add
// wraps at the top of the width, what AddUnless does below, at and past its limit, with negative
// values and when the sum passes the top of the width, and what DecIfPositive does from 2, 1, 0
// and -1. Values of 64 bits print as their high and low words. Last, a thread on each user core
// adds 1 to one counter 100000 times with AddAndGet32, and the test prints whether the counter
// ends at 300000.
//
// On a PS Vita TV on firmware 3.65 every AndGet call returns the new value, and every GetAnd
// call, CompareAndSet and DecIfPositive return the old value. CompareAndSet returns the old
// value whether it swaps or not, and Clear clears the bits of the value. AddUnless is an add
// with a limit, its last argument: it adds the value to the old one and, if the sum is at least
// the limit, stores the limit and returns 1, else stores the sum and returns 0. The compare is
// signed. At 32 and 64 bits the sum wraps at the top of the width before the compare, so a sum
// past the top is stored, while at 8 and 16 bits it doesn't wrap and the limit is stored.
// DecIfPositive leaves 0 and negative values alone at 32 and 64 bits, but at 8 and 16 bits it
// takes 0 down to -1. The counter ends at 300000.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static void show(const char *what, int bits, long long value) {
    if (bits == 64) {
        PRINTF((int)(value >> 32), "%d bits: %s, high word", bits, what);
        PRINTF((int)value, "%d bits: %s, low word", bits, what);
    } else {
        PRINTF((int)value, "%d bits: %s", bits, what);
    }
}

// One operation that returns a value: the store starts at start, then the call runs
#define OP(BITS, T, NAME, START, ...)                                                                  \
    do {                                                                                               \
        store = (T)(START);                                                                            \
        const long long r = (long long)sceKernelAtomic##NAME##BITS(&store, __VA_ARGS__);               \
        show(#NAME " from " #START " with " #__VA_ARGS__ ": the result", BITS, r);                     \
        show(#NAME " from " #START " with " #__VA_ARGS__ ": the value after", BITS, (long long)store); \
    } while (0)

// One operation that returns nothing
#define VOID_OP(BITS, T, NAME, START, ...)                                                             \
    do {                                                                                               \
        store = (T)(START);                                                                            \
        sceKernelAtomic##NAME##BITS(&store, __VA_ARGS__);                                              \
        show(#NAME " from " #START " with " #__VA_ARGS__ ": the value after", BITS, (long long)store); \
    } while (0)

// DecIfPositive, which takes no value
#define DEC(BITS, T, START)                                                             \
    do {                                                                                \
        store = (T)(START);                                                             \
        const long long r = (long long)sceKernelAtomicDecIfPositive##BITS(&store);      \
        show("DecIfPositive from " #START ": the result", BITS, r);                     \
        show("DecIfPositive from " #START ": the value after", BITS, (long long)store); \
    } while (0)

#define WIDTH(BITS, T, MAX)                           \
    static void test_##BITS(void) {                   \
        T store;                                      \
        VOID_OP(BITS, T, Set, 0x11, 0x22);            \
        OP(BITS, T, CompareAndSet, 0x11, 0x11, 0x33); \
        OP(BITS, T, CompareAndSet, 0x11, 0x12, 0x33); \
        OP(BITS, T, AddAndGet, 5, 3);                 \
        OP(BITS, T, AddAndGet, MAX, 1);               \
        OP(BITS, T, AddUnless, 5, 3, 100);            \
        OP(BITS, T, AddUnless, 5, 3, 8);              \
        OP(BITS, T, AddUnless, 5, 3, 7);              \
        OP(BITS, T, AddUnless, 100, 3, 7);            \
        OP(BITS, T, AddUnless, 0, -1, 5);             \
        OP(BITS, T, AddUnless, -10, 3, -8);           \
        OP(BITS, T, AddUnless, MAX, 1, MAX);          \
        OP(BITS, T, SubAndGet, 5, 3);                 \
        OP(BITS, T, AndAndGet, 0x5C, 0x36);           \
        OP(BITS, T, OrAndGet, 0x5C, 0x36);            \
        OP(BITS, T, XorAndGet, 0x5C, 0x36);           \
        OP(BITS, T, ClearAndGet, 0x5C, 0x36);         \
        OP(BITS, T, GetAndSet, 0x5C, 0x36);           \
        OP(BITS, T, GetAndAdd, 0x5C, 0x36);           \
        OP(BITS, T, GetAndSub, 0x5C, 0x36);           \
        OP(BITS, T, GetAndAnd, 0x5C, 0x36);           \
        OP(BITS, T, GetAndOr, 0x5C, 0x36);            \
        OP(BITS, T, GetAndXor, 0x5C, 0x36);           \
        OP(BITS, T, GetAndClear, 0x5C, 0x36);         \
        VOID_OP(BITS, T, ClearMask, 0x5C, 0x36);      \
        DEC(BITS, T, 2);                              \
        DEC(BITS, T, 1);                              \
        DEC(BITS, T, 0);                              \
        DEC(BITS, T, -1);                             \
    }

WIDTH(8, SceInt8, 0x7F)
WIDTH(16, SceInt16, 0x7FFF)
WIDTH(32, SceInt32, 0x7FFFFFFF)
WIDTH(64, SceInt64, 0x7FFFFFFFFFFFFFFFLL)

static SceInt32 counter;

static int adder_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (int i = 0; i < 100000; i++)
        sceKernelAtomicAddAndGet32(&counter, 1);
    return 0;
}

int main(void) {
    test_8();
    test_16();
    test_32();
    test_64();

    static const SceUInt32 masks[3] = { SCE_KERNEL_CPU_MASK_USER_0, SCE_KERNEL_CPU_MASK_USER_1, SCE_KERNEL_CPU_MASK_USER_2 };
    SceUID adders[3];
    for (int core = 0; core < 3; core++) {
        adders[core] = sceKernelCreateThread("adder", adder_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, masks[core], NULL);
        sceKernelStartThread(adders[core], 0, NULL);
    }
    for (int core = 0; core < 3; core++)
        test_join_thread(adders[core]);
    PRINT("three cores adding 100000 each: the counter ends at 300000", counter == 300000);
    test_exit();
    return 0;
}
