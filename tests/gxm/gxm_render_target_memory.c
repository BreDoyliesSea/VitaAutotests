// What sceGxmGetRenderTargetMemSize reports and which driver memory blocks sceGxmCreateRenderTarget
// takes. No scene starts, and each render target that is made is destroyed. The valid parameters
// are 960 by 544 pixels, one scene per frame, no multisampling and no flags. The cases:
// - the driver memory size for the valid parameters, for 4X and 2X multisampling, 2 and 8 scenes
//   per frame, 480 by 272, 1920 by 1088, 4096 by 4096, 32 by 32 and 1 by 1. The sizes are printed.
// - a render target with driverMemBlock -1, and whether sceGxmRenderTargetGetDriverMemBlock gives
//   -1 or a UID for it
// - a render target in each driver memory block: a USER_RW_UNCACHE block of the size the size call
//   gives rounded up to 4 KiB and one 4 KiB bigger, a cached USER_RW block, a USER_CDRAM_RW block
//   and a USER_MAIN_PHYCONT_NC_RW block, each rounded up to its unit. For each render target that
//   is made: whether sceGxmRenderTargetGetDriverMemBlock gives the block's UID, whether the app can
//   map the block with sceGxmMapMemory while it lives, and after the destroy, whether the block
//   maps and its free.
// - a USER_RW_UNCACHE block the app mapped before, one block for two render targets at once, the
//   UID of a freed block and the UID of a semaphore
// - the size call and a create with 9 and 0 scenes per frame, multisample mode 3, flags 0x4 and
//   0x10000, a width and a height of 4097, and the size call with a width and a height of 0
// Then, since a wrong implementation may crash on them, a block for code of the size rounded up to
// 1 MiB and a USER_RW_UNCACHE block 4 KiB smaller than the size, the same way as the other blocks,
// NULL pointers, and last a create with a width of 0.
//
// On a PS Vita TV on firmware 3.65 the size for the valid parameters is 0x2D000, 0x5D000 with 2X
// and 0x8D000 with 4X multisampling, 0x47000 with 2 and 0xEC000 with 8 scenes per frame, 0x75000
// for 1920 by 1088, 0x213000 for 4096 by 4096, and 0x15000 for 480 by 272, 32 by 32 and 1 by 1
// alike. A render target with driverMemBlock -1 reports -1 as its block. One in a USER_RW_UNCACHE
// block of the size or 4 KiB bigger, or in a USER_MAIN_PHYCONT_NC_RW block, works and reports the
// block, which the app can't map while it lives, with 0x805B0017 (DRIVER), but can after the
// destroy. A cached USER_RW block, a CDRAM block, a block for code and a block the app mapped fail
// with DRIVER. A block 4 KiB smaller, one another render target uses, a freed block's UID and a
// semaphore's UID fail with 0x805B0003 (INVALID_VALUE), as does every bad parameter in both calls.
// NULL pointers and a destroy of NULL give 0x805B0004 (INVALID_POINTER).
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

// vitasdk has a stub for it but no prototype
int sceKernelFreeMemBlockForVM(SceUID uid);

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

static SceGxmRenderTargetParams valid_params(void) {
    SceGxmRenderTargetParams params = { 0 };
    params.flags = 0;
    params.width = 960;
    params.height = 544;
    params.scenesPerFrame = 1;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.multisampleLocations = 0;
    params.driverMemBlock = -1;
    return params;
}

static SceSize round_up(SceSize size, SceSize unit) {
    return (size + unit - 1) / unit * unit;
}

static void *base_of(SceUID uid) {
    void *base = NULL;
    sceKernelGetMemBlockBase(uid, &base);
    return base;
}

// Prints the driver memory size for the parameters, or the error
static void size_of(const char *what, const SceGxmRenderTargetParams *params) {
    unsigned int size = 0;
    const int got = sceGxmGetRenderTargetMemSize(params, &size);
    PRINTF(got, "size for %s", what);
    if (got == 0)
        PRINTF(size, "size for %s: the size", what);
}

static void sizes(void) {
    SceGxmRenderTargetParams params = valid_params();
    size_of("the valid parameters", &params);
    params.multisampleMode = SCE_GXM_MULTISAMPLE_4X;
    size_of("4X multisampling", &params);
    params.multisampleMode = SCE_GXM_MULTISAMPLE_2X;
    size_of("2X multisampling", &params);
    params = valid_params();
    params.scenesPerFrame = 2;
    size_of("2 scenes per frame", &params);
    params.scenesPerFrame = 8;
    size_of("8 scenes per frame", &params);
    static const struct {
        uint16_t width, height;
    } dims[] = { { 480, 272 }, { 1920, 1088 }, { 4096, 4096 }, { 32, 32 }, { 1, 1 } };
    for (unsigned int i = 0; i < sizeof(dims) / sizeof(dims[0]); i++) {
        params = valid_params();
        params.width = dims[i].width;
        params.height = dims[i].height;
        char what[32];
        sceClibSnprintf(what, sizeof(what), "%u by %u", dims[i].width, dims[i].height);
        size_of(what, &params);
    }
}

// Makes a render target with the parameters, prints the result and destroys one that was made
static void create_once(const char *what, const SceGxmRenderTargetParams *params) {
    SceGxmRenderTarget *target = NULL;
    const int created = sceGxmCreateRenderTarget(params, &target);
    PRINTF(created, "create with %s", what);
    if (created == 0)
        PRINTF(sceGxmDestroyRenderTarget(target), "create with %s: destroy it", what);
}

static void default_block(void) {
    const SceGxmRenderTargetParams params = valid_params();
    SceGxmRenderTarget *target = NULL;
    const int created = sceGxmCreateRenderTarget(&params, &target);
    PRINT("create with driverMemBlock -1", created);
    if (created != 0)
        return;
    SceUID block = 0;
    PRINT("create with driverMemBlock -1: get its block",
          sceGxmRenderTargetGetDriverMemBlock(target, &block));
    PRINT("create with driverMemBlock -1: its block is -1", block == -1);
    PRINT("create with driverMemBlock -1: its block is a UID", block > 0);
    PRINT("create with driverMemBlock -1: destroy it", sceGxmDestroyRenderTarget(target));
}

// Maps the range, prints the result and unmaps a mapping that worked
static void map_once(const char *what, const char *when, void *base, SceSize size) {
    const int mapped = sceGxmMapMemory(base, size, RW);
    PRINTF(mapped, "%s: map the block %s", what, when);
    if (mapped == 0)
        sceGxmUnmapMemory(base);
}

// Makes a render target in the block, with what the block does around it, then frees the block
static void in_block(const char *what, SceUID block, SceSize size, int for_code) {
    PRINTF(test_created(block), "%s: allocate it", what);
    if (block < 0)
        return;
    char *const base = base_of(block);
    SceGxmRenderTargetParams params = valid_params();
    params.driverMemBlock = block;
    SceGxmRenderTarget *target = NULL;
    const int created = sceGxmCreateRenderTarget(&params, &target);
    PRINTF(created, "%s: create", what);
    if (created == 0) {
        SceUID got = -1;
        const int reported = sceGxmRenderTargetGetDriverMemBlock(target, &got);
        PRINTF(reported == 0 && got == block, "%s: it reports the block", what);
        map_once(what, "while it lives", base, size);
        PRINTF(sceGxmDestroyRenderTarget(target), "%s: destroy it", what);
        map_once(what, "after the destroy", base, size);
    }
    PRINTF(for_code ? sceKernelFreeMemBlockForVM(block) : sceKernelFreeMemBlock(block),
           "%s: free the block", what);
}

static SceUID alloc(SceUInt32 type, SceSize size) {
    return sceKernelAllocMemBlock("gxm render target", type, size, NULL);
}

static void blocks(SceSize size) {
    const SceSize page = round_up(size, 4 * KIB);
    in_block("an uncached block of the size", alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, page),
             page, 0);
    in_block("an uncached block 4 KiB bigger",
             alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, page + 4 * KIB), page + 4 * KIB, 0);
    in_block("a cached block", alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, page), page, 0);
    const SceSize cdram = round_up(size, 256 * KIB);
    in_block("a CDRAM block", alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, cdram), cdram, 0);
    const SceSize mib = round_up(size, MIB);
    in_block("a physically contiguous block",
             alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, mib), mib, 0);
}

static void mapped_before(SceSize size) {
    const SceSize page = round_up(size, 4 * KIB);
    const SceUID block = alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, page);
    char *const base = base_of(block);
    const int mapped = sceGxmMapMemory(base, page, RW);
    PRINT("a block the app mapped: map it", mapped);
    SceGxmRenderTargetParams params = valid_params();
    params.driverMemBlock = block;
    create_once("a block the app mapped", &params);
    if (mapped == 0)
        PRINT("a block the app mapped: unmap it", sceGxmUnmapMemory(base));
    PRINT("a block the app mapped: free it", sceKernelFreeMemBlock(block));
}

static void shared(SceSize size) {
    const SceSize page = round_up(size, 4 * KIB);
    const SceUID block = alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, page);
    SceGxmRenderTargetParams params = valid_params();
    params.driverMemBlock = block;
    SceGxmRenderTarget *first = NULL, *second = NULL;
    const int a = sceGxmCreateRenderTarget(&params, &first);
    const int b = sceGxmCreateRenderTarget(&params, &second);
    PRINT("one block for two: the first", a);
    PRINT("one block for two: the second", b);
    if (b == 0)
        PRINT("one block for two: destroy the second", sceGxmDestroyRenderTarget(second));
    if (a == 0)
        PRINT("one block for two: destroy the first", sceGxmDestroyRenderTarget(first));
    PRINT("one block for two: free it", sceKernelFreeMemBlock(block));
}

static void other_uids(SceSize size) {
    const SceUID block = alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, round_up(size, 4 * KIB));
    sceKernelFreeMemBlock(block);
    SceGxmRenderTargetParams params = valid_params();
    params.driverMemBlock = block;
    create_once("the UID of a freed block", &params);

    const SceUID sema = sceKernelCreateSema("gxm render target", 0, 0, 1, NULL);
    params.driverMemBlock = sema;
    create_once("the UID of a semaphore", &params);
    sceKernelDeleteSema(sema);
}

// The size call and a create with the parameters
static void both(const char *what, const SceGxmRenderTargetParams *params) {
    size_of(what, params);
    create_once(what, params);
}

static void bad_params(void) {
    SceGxmRenderTargetParams params = valid_params();
    params.scenesPerFrame = 9;
    both("9 scenes per frame", &params);
    params.scenesPerFrame = 0;
    both("0 scenes per frame", &params);
    params = valid_params();
    params.multisampleMode = 3;
    both("multisample mode 3", &params);
    params = valid_params();
    params.flags = 0x4;
    both("flags 0x4", &params);
    params.flags = 0x10000;
    both("flags 0x10000", &params);
    params = valid_params();
    params.width = 4097;
    both("a width of 4097", &params);
    params = valid_params();
    params.height = 4097;
    both("a height of 4097", &params);
}

int main(void) {
    PRINT("initialize libgxm", initialize());
    sizes();
    default_block();

    const SceGxmRenderTargetParams params = valid_params();
    unsigned int size = 0;
    if (sceGxmGetRenderTargetMemSize(&params, &size) != 0 || size == 0)
        size = 64 * KIB;
    blocks(size);
    mapped_before(size);
    shared(size);
    other_uids(size);
    bad_params();

    SceGxmRenderTargetParams zero = valid_params();
    zero.width = 0;
    size_of("a width of 0", &zero);
    zero = valid_params();
    zero.height = 0;
    size_of("a height of 0", &zero);

    // A block an app can't write while its domain is closed, and one too small for the render
    // target, which a wrong implementation may write to or past
    const SceSize mib = round_up(size, MIB);
    in_block("a block for code", sceKernelAllocMemBlockForVM("gxm render target vm", mib), mib, 1);
    const SceSize page = round_up(size, 4 * KIB);
    if (page > 4 * KIB)
        in_block("an uncached block 4 KiB smaller",
                 alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, page - 4 * KIB), page - 4 * KIB,
                 0);

    // NULL pointers go last, so an implementation that crashes on them loses nothing else
    SceGxmRenderTarget *target = NULL;
    PRINT("create with NULL parameters", sceGxmCreateRenderTarget(NULL, &target));
    PRINT("create with a NULL render target pointer", sceGxmCreateRenderTarget(&params, NULL));
    PRINT("size with NULL parameters", sceGxmGetRenderTargetMemSize(NULL, &size));
    PRINT("size with a NULL size pointer", sceGxmGetRenderTargetMemSize(&params, NULL));
    PRINT("destroy NULL", sceGxmDestroyRenderTarget(NULL));

    // Last, since a wrong implementation may crash making a render target 0 pixels wide
    zero = valid_params();
    zero.width = 0;
    create_once("a width of 0", &zero);
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
