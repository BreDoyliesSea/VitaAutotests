// What a thread's stack holds and how its stack free size follows from it: the lowest word, the
// fill above it, and the free size next to the lowest word above it that isn't 0xFFFFFFFF, read by
// another thread before the thread starts, while it waits, after it ended and in a second run.
// Also the main thread's stack and its size.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define STACK_SIZE 0x4000
#define FILL 0xFFFFFFFFU

static SceUID sema, main_sema;
static volatile SceUInt32 slot6, slot7;

// The index of the lowest word above the first one that isn't FILL, or count if there is none
static int lowest_used(const volatile SceUInt32 *words, int count) {
    int i = 1;
    while (i < count && words[i] == FILL)
        i++;
    return i;
}

static int fill_words(const volatile SceUInt32 *words, int count) {
    int n = 0;
    for (int i = 0; i < count; i++)
        n += words[i] == FILL;
    return n;
}

static int thread_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    slot6 = *(volatile SceUInt32 *)sceKernelGetTLSAddr(6);
    slot7 = *(volatile SceUInt32 *)sceKernelGetTLSAddr(7);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    return 0;
}

static SceUID main_thread;
static volatile int main_free, main_lowest, main_first_is_fill, main_fill_above, main_stack_size;

// Reads the main thread's stack while it waits
static int main_reader(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    TEST_WAIT_FOR(test_in_wait(main_thread, WAIT_TYPE_SEMA, main_sema));
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(main_thread, &info);
    const volatile SceUInt32 *words = (const volatile SceUInt32 *)info.stack;
    const int count = info.stackSize / 4;
    main_stack_size = info.stackSize;
    main_free = sceKernelGetThreadStackFreeSize(main_thread);
    main_lowest = lowest_used(words, count);
    main_first_is_fill = words[0] == FILL;
    main_fill_above = main_lowest > 1;
    sceKernelSignalSema(main_sema, 1);
    return 0;
}

static const volatile SceUInt32 *stack_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return (const volatile SceUInt32 *)info.stack;
}

int main(void) {
    const int words_count = STACK_SIZE / 4;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID thread = sceKernelCreateThread("fill", thread_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, STACK_SIZE, 0, 0, NULL);
    const volatile SceUInt32 *words = stack_of(thread);

    const SceUInt32 lowest_before = words[0];
    PRINT("before it starts: the lowest word is 0", lowest_before == 0);
    PRINT("before it starts: the lowest word is 0xFFFFFFFF", lowest_before == FILL);
    PRINT("before it starts: words that are 0xFFFFFFFF", fill_words(words, words_count));
    const int free_before = sceKernelGetThreadStackFreeSize(thread);
    PRINT("before it starts: the free size is the stack size", free_before == STACK_SIZE);
    PRINT("before it starts: the free size is 4 times the stack size", free_before == 4 * STACK_SIZE);
    PRINT("before it starts: the free size is 16 times the index of the lowest used word", free_before == 16 * lowest_used(words, words_count));

    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema));
    const int lowest_first = lowest_used(words, words_count);
    const int free_first = sceKernelGetThreadStackFreeSize(thread);
    PRINT("while it waits: the lowest word is what it was before the start", words[0] == lowest_before);
    PRINT("while it waits: the lowest word is TLS slot 6", words[0] == slot6);
    PRINT("while it waits: the lowest word is TLS slot 7", words[0] == slot7);
    PRINT("while it waits: the lowest used word is over 1 KiB above the low end", lowest_first * 4 > 0x400);
    PRINT("while it waits: the free size is 16 times the index of the lowest used word", free_first == 16 * lowest_first);
    sceKernelSignalSema(sema, 1);
    sceKernelWaitThreadEnd(thread, NULL, NULL);

    const int free_ended = sceKernelGetThreadStackFreeSize(thread);
    PRINT("after it ended: the lowest used word is the one it was while it waited", lowest_used(words, words_count) == lowest_first);
    PRINT("after it ended: the free size is 16 times the index of the lowest used word", free_ended == 16 * lowest_used(words, words_count));
    PRINT("after it ended: the lowest word is what it was before the start", words[0] == lowest_before);

    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema));
    const int lowest_second = lowest_used(words, words_count);
    PRINT("second run: the lowest word is what it was in the first run", words[0] == lowest_before);
    PRINT("second run: the lowest used word is the one from the first run", lowest_second == lowest_first);
    PRINT("second run: the free size is 16 times the index of the lowest used word", sceKernelGetThreadStackFreeSize(thread) == 16 * lowest_second);
    sceKernelSignalSema(sema, 1);
    sceKernelWaitThreadEnd(thread, NULL, NULL);

    const SceUID other = sceKernelCreateThread("other", thread_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, STACK_SIZE, 0, 0, NULL);
    PRINT("another thread: its lowest word is different", stack_of(other)[0] != lowest_before);
    sceKernelDeleteThread(other);
    sceKernelDeleteThread(thread);

    main_thread = sceKernelGetThreadId();
    main_sema = sceKernelCreateSema("main", 0, 0, 1, NULL);
    const SceUID reader = sceKernelCreateThread("reader", main_reader, SCE_KERNEL_DEFAULT_PRIORITY_USER, STACK_SIZE, 0, 0, NULL);
    sceKernelStartThread(reader, 0, NULL);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(main_sema, 1, &timeout);
    sceKernelWaitThreadEnd(reader, NULL, NULL);
    PRINT("main thread: its stack size is the default for a main thread", main_stack_size == SCE_KERNEL_THREAD_STACK_SIZE_DEFAULT_USER_MAIN);
    PRINT("main thread: the lowest word is 0xFFFFFFFF", main_first_is_fill);
    PRINT("main thread: words above the lowest one are 0xFFFFFFFF", main_fill_above);
    PRINT("main thread: the free size is 16 times the index of the lowest used word", main_free == 16 * main_lowest);

    test_exit();
    return 0;
}
