// How sceKernelGetThreadRunStatus takes its arguments. The vitasdk declares it with a thread ID
// first and the struct second, but thread_info_fields and readonly_out_info that call it that way
// fail with 0x80020006, as if the struct pointer were NULL. This test calls it with the struct as the
// first and only argument. The struct sits in a buffer of 64 words with room behind it, each word set
// to a marker before the call, and the size word set to the size the case names. Cases:
// - the struct with its size word set to the size of the struct: the call's result, whether the size
//   word is unchanged, whether a word of the struct changed, whether a word past the struct changed,
//   and what the entries of the four cores hold: how many hold this thread, and for the one that
//   does, whether its process is this one and its priority is this thread's
// - the size word set to 0
// - the size word set to 4 bytes more than the struct
// - NULL as the struct
//
// On a PS Vita TV on firmware 3.65 the call with the struct and its size works and returns 0. It
// fills the struct, writes nothing past it, keeps the size word, and exactly one core's entry holds
// this thread, with this process and this thread's priority. With a size of 0 it returns 0 and
// writes nothing. With a size past the struct it fails with 0x8002710B, like the info calls, and
// writes nothing. NULL gives 0x80020006.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define WORDS 64
#define STRUCT_WORDS (sizeof(SceKernelThreadRunStatus) / sizeof(unsigned int))
#define MARKER 0xA5A5A5A5u

static union {
    SceKernelThreadRunStatus status;
    unsigned int words[WORDS];
} buffer;

// Sets every word to the marker, then the size word to size
static void fill(SceSize size) {
    for (unsigned int i = 0; i < WORDS; i++)
        buffer.words[i] = MARKER;
    buffer.status.size = size;
}

// Returns whether any word from first up to last isn't the marker
static int changed(unsigned int first, unsigned int last) {
    for (unsigned int i = first; i < last; i++) {
        if (buffer.words[i] != MARKER)
            return 1;
    }
    return 0;
}

// Prints what a call with the size word set to size did to the buffer
static void print_buffer(const char *name, SceSize size, int result) {
    PRINTF(result, "%s: the call", name);
    PRINTF(buffer.status.size == size, "%s: the size word is unchanged", name);
    PRINTF(changed(1, STRUCT_WORDS), "%s: a word of the struct changed", name);
    PRINTF(changed(STRUCT_WORDS, WORDS), "%s: a word past the struct changed", name);
}

// Prints which core entries of the filled struct hold the calling thread
static void print_entries(const char *name) {
    const SceUID self = sceKernelGetThreadId();
    int holders = 0;
    int process_matches = 0;
    int priority_matches = 0;
    for (int i = 0; i < 4; i++) {
        if (buffer.status.cpuInfo[i].threadId != self)
            continue;
        holders++;
        process_matches = buffer.status.cpuInfo[i].processId == sceKernelGetProcessId();
        priority_matches = buffer.status.cpuInfo[i].priority == sceKernelGetThreadCurrentPriority();
    }
    PRINTF(holders, "%s: the entries that hold this thread", name);
    PRINTF(process_matches, "%s: its process is this one", name);
    PRINTF(priority_matches, "%s: its priority is this thread's", name);
}

int main(void) {
    fill(sizeof(SceKernelThreadRunStatus));
    int result = get_thread_run_status(&buffer.status);
    print_buffer("size of the struct", sizeof(SceKernelThreadRunStatus), result);
    print_entries("size of the struct");

    fill(0);
    result = get_thread_run_status(&buffer.status);
    print_buffer("size 0", 0, result);

    fill(sizeof(SceKernelThreadRunStatus) + 4);
    result = get_thread_run_status(&buffer.status);
    print_buffer("size past the struct", sizeof(SceKernelThreadRunStatus) + 4, result);

    PRINT("NULL", get_thread_run_status(NULL));

    test_exit();
    return 0;
}
