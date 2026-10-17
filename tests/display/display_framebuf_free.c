// Freeing the memory block of the framebuffer the display shows. The valid framebuffer is a black
// 960 by 544 one in CDRAM, like in the display test. The test sets a framebuffer like it in another
// 2 MiB CDRAM block, waits for it to take effect, and frees that block while the display shows it.
// Then it prints whether sceDisplayGetFrameBuf still reports the freed framebuffer, sets the valid
// framebuffer, waits for it, prints whether the display reports that one, and frees the block
// again if the first free failed. It's apart from display_framebuf_blocks, which frees each block
// only after setting the valid framebuffer back, since a wrong implementation may crash once the
// display reads the freed memory.
//
// On a PS Vita TV on firmware 3.65 the free of the block works while the display shows the
// framebuffer in it, and sceDisplayGetFrameBuf still reports the freed framebuffer after the free.
// Setting the valid framebuffer then works, and the display reports that one. So a set of a
// framebuffer where a block was freed fails, as display_framebuf_blocks shows, but the free of a
// block the display already shows works.
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

// Whether the display reports a framebuffer at base
static int reports(void *base) {
    SceDisplayFrameBuf got = { .size = sizeof(got) };
    return sceDisplayGetFrameBuf(&got, SCE_DISPLAY_SETBUF_NEXTFRAME) == 0 && got.base == base &&
           got.pitch == WIDTH && got.width == WIDTH && got.height == HEIGHT;
}

int main(void) {
    const SceUID valid = sceKernelAllocMemBlock("display valid",
                                                SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                                BLOCK_SIZE, NULL);
    void *const valid_base = base_of(valid);
    sceClibMemset(valid_base, 0, BLOCK_SIZE);
    SceDisplayFrameBuf fb = framebuffer(valid_base);
    PRINT("set the valid framebuffer", sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME));
    PRINT("wait for it", sceDisplayWaitSetFrameBuf());

    const SceUID block = sceKernelAllocMemBlock("display freed",
                                                SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                                BLOCK_SIZE, NULL);
    PRINT("allocate another block", test_created(block));
    void *const base = base_of(block);
    sceClibMemset(base, 0, BLOCK_SIZE);
    fb = framebuffer(base);
    const int set = sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    PRINT("set a framebuffer in it", set);
    PRINT("wait for it", sceDisplayWaitSetFrameBuf());
    PRINT("the display reports it", reports(base));

    const int freed = sceKernelFreeMemBlock(block);
    PRINT("free its block while the display shows it", freed);
    PRINT("after the free: the display still reports it", reports(base));

    fb = framebuffer(valid_base);
    PRINT("set the valid framebuffer again",
          sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME));
    PRINT("wait for it", sceDisplayWaitSetFrameBuf());
    PRINT("the display reports the valid framebuffer", reports(valid_base));
    if (freed != 0)
        PRINT("free the block again", sceKernelFreeMemBlock(block));

    test_exit();
    return 0;
}
