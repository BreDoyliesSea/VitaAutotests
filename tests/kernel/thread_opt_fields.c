// sceKernelCreateThread's option struct with values that are allowed for a game. The attr bits 0x1
// to 0x10 ask for the fields behind them, so each case sets one or more of them with a field value
// and creates a thread. A thread that was created is started, joined, and reports whether it ran.
//
// The cases are the user stack type with the type games may use and with 0x0E20D060, the TLS type
// with the type games may use, both of them together, the last field with the values 0x301, 0x1,
// 0x100, 0x200 and 0x2, the user stack type written past the size of the struct and at its very
// end, and attr 0x22 where the bit 0x20 stands for the whole struct being ignored.
//
// On a PS Vita TV on firmware 3.65 the user stack type 0x0C20D060, the TLS type 0x0C20D060 and both
// together create a thread that runs. The user stack type 0x0E20D060 gives 0x8002000C. The last
// field takes 0x301, 0x1, 0x100 and 0x200 and gives 0x8002000C for 0x2. A user stack type past the
// size of the struct reads as 0 and gives 0x8002000C, one at the end of the size works, and attr
// 0x22 ignores its fields, so the thread is created and runs.
#include <psp2/kernel/threadmgr.h>

#include "test.h"

// The memory type of user read and write memory, which a game may ask for, and 0x0E20D060, which a
// game may not
#define MEMTYPE_USER_RW 0x0C20D060
#define MEMTYPE_SYSTEM_RW 0x0E20D060

static volatile int ran;

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    ran = 1;
    return 0;
}

// Big enough for any size the test claims, all zero but for what a case sets
static union {
    SceKernelThreadOptParam opt;
    unsigned char bytes[0x100];
} buf;

// Starts a case with an option struct of this size and attr, and every other field 0
static void begin(SceSize size, SceUInt32 attr) {
    sceClibMemset(&buf, 0, sizeof(buf));
    buf.opt.size = size;
    buf.opt.attr = attr;
}

// Creates a thread with the option struct in buf. A thread that was created is started and joined.
static void create_case(const char *name) {
    ran = 0;
    const SceUID thread = sceKernelCreateThread("opt", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, &buf.opt);
    PRINTF(test_created(thread), "%s: create", name);
    if (thread < 0)
        return;
    PRINTF(sceKernelStartThread(thread, 0, NULL), "%s: start", name);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitThreadEnd(thread, NULL, &timeout);
    PRINTF(ran, "%s: it ran", name);
    sceKernelDeleteThread(thread);
}

int main(void) {
    begin(sizeof(buf.opt), 0x2);
    buf.opt.uStackMemType = MEMTYPE_USER_RW;
    create_case("attr 0x2 with the user stack type USER_RW");

    begin(sizeof(buf.opt), 0x2);
    buf.opt.uStackMemType = MEMTYPE_SYSTEM_RW;
    create_case("attr 0x2 with the user stack type 0x0E20D060");

    begin(sizeof(buf.opt), 0x4);
    buf.opt.uTLSMemType = MEMTYPE_USER_RW;
    create_case("attr 0x4 with the TLS type USER_RW");

    begin(sizeof(buf.opt), 0x6);
    buf.opt.uStackMemType = MEMTYPE_USER_RW;
    buf.opt.uTLSMemType = MEMTYPE_USER_RW;
    create_case("attr 0x6 with both types USER_RW");

    static const SceUInt32 last_fields[] = { 0x301, 0x1, 0x100, 0x200, 0x2 };
    for (unsigned int i = 0; i < sizeof(last_fields) / sizeof(last_fields[0]); i++) {
        char name[64];
        sceClibSnprintf(name, sizeof(name), "attr 0x10 with the last field 0x%X", (unsigned)last_fields[i]);
        begin(sizeof(buf.opt), 0x10);
        buf.opt.data_0x18 = last_fields[i];
        create_case(name);
    }

    // The user stack type is at +0xC, so a size of 8 ends before it and a size of 0x10 includes it
    begin(8, 0x2);
    buf.opt.uStackMemType = MEMTYPE_USER_RW;
    create_case("size 8, attr 0x2 and USER_RW at +0xC");

    begin(0x10, 0x2);
    buf.opt.uStackMemType = MEMTYPE_USER_RW;
    create_case("size 0x10, attr 0x2 and USER_RW at +0xC");

    // Bit 0x20 makes the create ignore the struct, whatever else it holds
    begin(sizeof(buf.opt), 0x22);
    create_case("attr 0x22 with the user stack type 0");

    test_exit();
    return 0;
}
