// Which memory sceDisplaySetFrameBuf takes for a framebuffer, and whether sceDisplayGetFrameBuf
// then reports it. A framebuffer here is 960 by 544 with a pitch of 960, A8B8G8R8, at the base of a
// fresh 2 MiB block unless the case says otherwise. The valid framebuffer is a black one in CDRAM,
// which the display test shows the display takes. After each set that works the test waits for it
// to take effect and prints whether sceDisplayGetFrameBuf reports its base, pitch and size, then
// sets the valid framebuffer back and waits for that before it frees anything. The cases:
// - a block of each type: USER_RW, USER_RW_UNCACHE, USER_CDRAM_RW, USER_CDRAM_L1WBWA_RW,
//   USER_CDRAM_R, USER_MAIN_PHYCONT_RW, USER_MAIN_PHYCONT_NC_RW, USER_MAIN_R, a block opened with
//   0x30 from a USER_RW_UNCACHE block, and a block for code. Each is set, then mapped with
//   sceGxmMapMemory, with READ for the read only types and READ and WRITE for the others, and set
//   again while it's mapped.
// - a CDRAM framebuffer 16, 64, 128, 512 and 4096 bytes in, and one 8 KiB in, which ends at the
//   block's end
// Last, since a wrong implementation may read it, a framebuffer where a block was freed. The valid
// framebuffer is set after it.
//
// On a PS Vita TV on firmware 3.65 a framebuffer in a block of every type works, the read only
// types, the opened block and the block for code too, before and while the block is mapped, and the
// display reports each one. The block for code doesn't map, with 0x805B0017 (DRIVER), and the set
// after works anyway. A CDRAM framebuffer 16, 64 or 128 bytes in fails with 0x80290002
// (INVALID_ADDR), while 512 and 4096 bytes in work, as does one that ends at the block's end. A
// framebuffer where a block was freed fails with 0x80022007 (VA2PA_FAULT).
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define WIDTH 960
#define HEIGHT 544
#define BLOCK_SIZE (2 * MIB)
#define ENABLE_OPEN 0x4000

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

// vitasdk has a stub for it but no prototype
int sceKernelFreeMemBlockForVM(SceUID uid);

// The option struct games pass, without the fields vitasdk adds after strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static void *valid_base;

static void display_callback(const void *data) {
    (void)data;
}

static int initialize(void) {
    SceGxmInitializeParams init = { 0 };
    init.displayQueueMaxPendingCount = 2;
    init.displayQueueCallback = display_callback;
    init.displayQueueCallbackDataSize = 4;
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    return sceGxmInitialize(&init);
}

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

// Sets the valid framebuffer and waits until the display shows it
static void set_valid(void) {
    SceDisplayFrameBuf fb = framebuffer(valid_base);
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitSetFrameBuf();
}

// Sets a framebuffer at base and prints the result. If it works, prints whether the display reports
// it once it shows it, then sets the valid framebuffer back.
static void set_at(const char *what, void *base) {
    SceDisplayFrameBuf fb = framebuffer(base);
    const int set = sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    PRINTF(set, "%s", what);
    if (set != 0)
        return;
    sceDisplayWaitSetFrameBuf();
    SceDisplayFrameBuf got = { .size = sizeof(got) };
    const int get = sceDisplayGetFrameBuf(&got, SCE_DISPLAY_SETBUF_NEXTFRAME);
    PRINTF(get == 0 && got.base == base && got.pitch == WIDTH && got.width == WIDTH &&
               got.height == HEIGHT && got.pixelformat == SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
           "%s: the display reports it", what);
    set_valid();
}

// Sets a framebuffer in the block, then maps the block and sets it again
static void set_in(const char *what, void *base, SceUInt32 attribs) {
    char label[96];
    sceClibSnprintf(label, sizeof(label), "%s: set it", what);
    set_at(label, base);
    const int mapped = sceGxmMapMemory(base, BLOCK_SIZE, attribs);
    PRINTF(mapped, "%s: map it", what);
    sceClibSnprintf(label, sizeof(label), "%s: set it while it's mapped", what);
    set_at(label, base);
    if (mapped == 0)
        sceGxmUnmapMemory(base);
}

static void block_type(const char *what, SceUInt32 type, int writable) {
    const SceUID block = sceKernelAllocMemBlock("display block", type, BLOCK_SIZE, NULL);
    PRINTF(test_created(block), "%s: allocate it", what);
    if (block < 0)
        return;
    void *const base = base_of(block);
    if (writable)
        sceClibMemset(base, 0, BLOCK_SIZE);
    set_in(what, base, writable ? RW : SCE_GXM_MEMORY_ATTRIB_READ);
    sceKernelFreeMemBlock(block);
}

static void opened(void) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID original =
        sceKernelAllocMemBlock("display opened", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                               BLOCK_SIZE, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("an opened block: allocate its original", test_created(original));
    if (original < 0)
        return;
    sceClibMemset(base_of(original), 0, BLOCK_SIZE);
    const SceUID block = sceKernelOpenMemBlock("display opened", 0x30);
    PRINT("an opened block: open it", test_created(block));
    if (block >= 0) {
        set_in("an opened block", base_of(block), RW);
        sceKernelCloseMemBlock(block);
    }
    sceKernelFreeMemBlock(original);
}

static void for_code(void) {
    const SceUID block = sceKernelAllocMemBlockForVM("display vm", BLOCK_SIZE);
    PRINT("a block for code: allocate it", test_created(block));
    if (block < 0)
        return;
    set_in("a block for code", base_of(block), RW);
    sceKernelFreeMemBlockForVM(block);
}

static void offsets(void) {
    const SceUID block = sceKernelAllocMemBlock("display offsets",
                                                SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                                BLOCK_SIZE, NULL);
    char *const base = base_of(block);
    sceClibMemset(base, 0, BLOCK_SIZE);
    static const SceSize bys[] = { 16, 64, 128, 512, 4 * KIB };
    for (unsigned int i = 0; i < sizeof(bys) / sizeof(bys[0]); i++) {
        char what[64];
        sceClibSnprintf(what, sizeof(what), "a CDRAM framebuffer %u bytes in", (unsigned)bys[i]);
        set_at(what, base + bys[i]);
    }
    // The framebuffer takes 0x1FE000 bytes, so 8 KiB in it ends at the end of the 2 MiB block
    set_at("a CDRAM framebuffer that ends at the block's end",
           base + BLOCK_SIZE - WIDTH * HEIGHT * 4);
    sceKernelFreeMemBlock(block);
}

int main(void) {
    const SceUID valid = sceKernelAllocMemBlock("display valid",
                                                SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                                BLOCK_SIZE, NULL);
    valid_base = base_of(valid);
    sceClibMemset(valid_base, 0, BLOCK_SIZE);
    SceDisplayFrameBuf fb = framebuffer(valid_base);
    PRINT("set the valid framebuffer", sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME));
    sceDisplayWaitSetFrameBuf();
    PRINT("initialize libgxm", initialize());

    block_type("USER_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 1);
    block_type("USER_RW_UNCACHE", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 1);
    block_type("USER_CDRAM_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 1);
    block_type("USER_CDRAM_L1WBWA_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_L1WBWA_RW, 1);
    block_type("USER_CDRAM_R", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_R, 0);
    block_type("USER_MAIN_PHYCONT_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, 1);
    block_type("USER_MAIN_PHYCONT_NC_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 1);
    block_type("USER_MAIN_R", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R, 0);
    opened();
    for_code();
    offsets();

    // Memory in no block goes last, since a wrong implementation may read it once it's displayed
    const SceUID gone = sceKernelAllocMemBlock("display gone",
                                               SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, BLOCK_SIZE,
                                               NULL);
    void *const gone_base = base_of(gone);
    sceKernelFreeMemBlock(gone);
    set_at("where a block was freed", gone_base);
    set_valid();

    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
