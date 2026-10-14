// Thread names: what thread info reports for names of 31, 32 and 255 characters, an empty name, two
// threads with the same name, bytes above 0x7F, and whether the name is copied at creation
#include <psp2/kernel/threadmgr.h>

#include "test.h"

static int entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

static char name_buf[256];

// Fills name_buf with len copies of c
static const char *make_name(char c, int len) {
    sceClibMemset(name_buf, c, len);
    name_buf[len] = '\0';
    return name_buf;
}

// Creates a thread named name, and prints the result, the length of the name its info reports and
// whether that name is the start of the given one
static void check_name(const char *label, const char *name) {
    const SceUID thread = sceKernelCreateThread(name, entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    PRINTF(test_created(thread), "%s: create", label);
    if (thread < 0)
        return;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    PRINTF(sceKernelGetThreadInfo(thread, &info), "%s: info", label);
    const int len = sceClibStrnlen(info.name, sizeof(info.name));
    PRINTF(len, "%s: the length of the name in its info", label);
    PRINTF(sceClibStrncmp(info.name, name, len) == 0, "%s: it is the start of the given name", label);
    sceKernelDeleteThread(thread);
}

int main(void) {
    check_name("31 characters", make_name('a', 31));
    check_name("32 characters", make_name('b', 32));
    check_name("255 characters", make_name('c', 255));
    check_name("an empty name", "");
    check_name("bytes above 0x7F", "\x80\xC3\xA9\xFE\xFF");

    // The bytes of the last name, one by one
    SceUID thread = sceKernelCreateThread("\x80\xC3\xA9\xFE\xFF", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    for (int i = 0; i < 6; i++)
        PRINTF((unsigned char)info.name[i], "bytes above 0x7F: byte %d of the name", i);
    sceKernelDeleteThread(thread);

    // Two threads with the same name
    const SceUID first = sceKernelCreateThread("same", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    const SceUID second = sceKernelCreateThread("same", entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    PRINT("same name: create the first", test_created(first));
    PRINT("same name: create the second", test_created(second));
    PRINT("same name: they are different threads", first != second);
    SceKernelThreadInfo first_info = { .size = sizeof(first_info) }, second_info = { .size = sizeof(second_info) };
    sceKernelGetThreadInfo(first, &first_info);
    sceKernelGetThreadInfo(second, &second_info);
    PRINT("same name: both report it", sceClibStrcmp(first_info.name, "same") == 0 && sceClibStrcmp(second_info.name, "same") == 0);
    sceKernelStartThread(second, 0, NULL);
    SceUInt timeout = TEST_WAIT_US;
    PRINT("same name: the second runs", sceKernelWaitThreadEnd(second, NULL, &timeout));
    sceKernelDeleteThread(first);
    sceKernelDeleteThread(second);

    // Changing the buffer after the creation
    char buf[8] = "copied";
    thread = sceKernelCreateThread(buf, entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    sceClibStrncpy(buf, "changed", sizeof(buf));
    info.size = sizeof(info);
    sceKernelGetThreadInfo(thread, &info);
    PRINT("the name is copied at creation", sceClibStrcmp(info.name, "copied") == 0);
    sceKernelDeleteThread(thread);

    test_exit();
    return 0;
}
