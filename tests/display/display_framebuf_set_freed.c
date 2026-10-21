// Setting a framebuffer in memory where a block was freed. The valid framebuffer is a black 960 by
// 544 one at the base of a 2 MiB CDRAM block, like in the display test. The test allocates another
// 2 MiB CDRAM block, frees it, and sets a framebuffer at its old base. If that works, it waits for
// the framebuffer to take effect and prints whether sceDisplayGetFrameBuf reports it. The valid
// framebuffer is set after it. It's apart from display_framebuf_blocks, since a wrong
// implementation may crash once the display reads the freed memory.
//
// On a PS Vita TV on firmware 3.65 the set of a framebuffer where a block was freed fails with
// 0x80022007 (VA2PA_FAULT).
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define WIDTH 960
#define HEIGHT 544
#define BLOCK_SIZE 0x200000

static void *base_of(SceUID uid) {
    void *base = NULL;
    sceKernelGetMemBlockBase(uid, &base);
    return base;
}

static SceDisplayFrameBuf framebuffer(void *base) {
    const SceDisplayFrameBuf fb = { sizeof(SceDisplayFrameBuf), base, WIDTH,
                                    SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, WIDTH, HEIGHT };
    return fb;
}

int main(void) {
    const SceUID valid = sceKernelAllocMemBlock("display valid",
                                                SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                                BLOCK_SIZE, NULL);
    void *const valid_base = base_of(valid);
    sceClibMemset(valid_base, 0, BLOCK_SIZE);
    SceDisplayFrameBuf fb = framebuffer(valid_base);
    PRINT("set the valid framebuffer", sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME));
    sceDisplayWaitSetFrameBuf();

    const SceUID gone = sceKernelAllocMemBlock("display gone",
                                               SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, BLOCK_SIZE,
                                               NULL);
    void *const gone_base = base_of(gone);
    sceKernelFreeMemBlock(gone);

    fb = framebuffer(gone_base);
    const int set = sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    PRINTF(set, "where a block was freed");
    if (set == 0) {
        sceDisplayWaitSetFrameBuf();
        SceDisplayFrameBuf got = { .size = sizeof(got) };
        const int get = sceDisplayGetFrameBuf(&got, SCE_DISPLAY_SETBUF_NEXTFRAME);
        PRINTF(get == 0 && got.base == gone_base && got.pitch == WIDTH && got.width == WIDTH &&
                   got.height == HEIGHT && got.pixelformat == SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
               "where a block was freed: the display reports it");
    }

    fb = framebuffer(valid_base);
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitSetFrameBuf();

    test_exit();
    return 0;
}
