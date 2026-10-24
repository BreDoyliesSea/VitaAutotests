// The option struct of a mutex with the ceiling attribute 0x4, which holds a size and the ceiling
// priority. Each case creates a mutex with it, prints the create's result and, for a mutex that was
// created, the attribute and the ceiling its info reports, then deletes it. The lines that
// mutex_ceiling prints already are left out. The cases:
// - a ceiling of 0x80, 0x40 and 0 with an option size of 8, only the attribute in the info
// - a ceiling of 0xFF and of 0x100 with an option size of 8
// - a ceiling of 0x80 with an option size of 4, the ceiling in the info
// - a ceiling of 0x80 with an option size of 0
// - a recursive mutex with the ceiling attribute (0x6) and a ceiling of 0x80
//
// On a PS Vita TV on firmware 3.65 the info of a mutex reports the attribute as it was given, 0x4
// or 0x6, for every ceiling that was accepted. A ceiling of 0xFF or of 0x100 is refused with the
// priority error 0x80028023, like the 0xC0 that mutex_ceiling shows. A size of 4 or of 0 doesn't
// stop the struct from being read, so the mutex gets the ceiling 0x80 from the struct either way,
// and the recursive ceiling mutex gets it too.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// Lines mutex_ceiling prints already for a case
#define KNOWN_CREATE 0x1
#define KNOWN_CEILING 0x2

static const struct {
    const char *name;
    SceUInt32 attr;
    SceSize size;
    int ceiling;
    int known;
} cases[] = {
    { "ceiling 0x80, size 8", SCE_KERNEL_MUTEX_ATTR_CEILING, 8, 0x80, KNOWN_CREATE | KNOWN_CEILING },
    { "ceiling 0x40, size 8", SCE_KERNEL_MUTEX_ATTR_CEILING, 8, 0x40, KNOWN_CREATE | KNOWN_CEILING },
    { "ceiling 0, size 8", SCE_KERNEL_MUTEX_ATTR_CEILING, 8, 0, KNOWN_CREATE | KNOWN_CEILING },
    { "ceiling 0xFF, size 8", SCE_KERNEL_MUTEX_ATTR_CEILING, 8, 0xFF, 0 },
    { "ceiling 0x100, size 8", SCE_KERNEL_MUTEX_ATTR_CEILING, 8, 0x100, 0 },
    { "ceiling 0x80, size 4", SCE_KERNEL_MUTEX_ATTR_CEILING, 4, 0x80, KNOWN_CREATE },
    { "ceiling 0x80, size 0", SCE_KERNEL_MUTEX_ATTR_CEILING, 0, 0x80, 0 },
    { "recursive ceiling 0x80, size 8", SCE_KERNEL_MUTEX_ATTR_CEILING | SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 8, 0x80, 0 },
};

int main(void) {
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        SceKernelMutexOptParam opt = { .size = cases[i].size, .ceilingPriority = cases[i].ceiling };
        const SceUID mutex = sceKernelCreateMutex("ceiling", cases[i].attr, 0, &opt);
        // A create that fails is printed even where a pass is known, so the line shows up
        if (!(cases[i].known & KNOWN_CREATE) || mutex < 0)
            PRINTF(test_created(mutex), "%s: create", cases[i].name);
        if (mutex < 0)
            continue;
        MutexInfo info = { .size = sizeof(info) };
        sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&info);
        PRINTF(info.attr, "%s: the attribute in its info", cases[i].name);
        if (!(cases[i].known & KNOWN_CEILING))
            PRINTF(info.ceilingPriority, "%s: the ceiling in its info", cases[i].name);
        sceKernelDeleteMutex(mutex);
    }

    test_exit();
    return 0;
}
