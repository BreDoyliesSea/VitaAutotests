// What sceGxmColorSurfaceInit and sceGxmDepthStencilSurfaceInit take for the memory a surface
// describes, and whether the surface keeps the pointers it was given. No scene starts, so the GPU
// never touches the memory. The valid color surface is 960 by 544 pixels, linear, U8U8U8U8_ABGR,
// with a stride of 960. The valid depth stencil surface is tiled S8D24 with a stride of 960
// samples, with its depth and stencil data 2 MiB apart. Their data is at the base of a 4 MiB
// USER_RW_UNCACHE block mapped with READ and WRITE unless the case says otherwise. For both
// surfaces, each init that works prints whether the surface keeps the data pointer and the stride:
// sceGxmColorSurfaceGetData and sceGxmColorSurfaceGetStrideInPixels for a color surface, and the
// depthData and stencilData fields and sceGxmDepthStencilSurfaceGetStrideInSamples for a depth
// stencil surface. The cases:
// - both valid surfaces, then each with its data in a block that isn't mapped, in a block mapped
//   with READ only, in a USER_MAIN_R block, in a block for code and on the stack
// - data 1, 2, 4, 8, 16, 32 and 64 bytes into the block, and for the depth stencil surface the
//   stencil data 1, 4 and 16 bytes past its place
// - data in the block's last 4 KiB, so the surface runs past the block's end
// - color surface strides of 961, 962, 964, 968, 1024, 959, which is under the width, and 0, sizes
//   of 0 by 544, 960 by 0, 4096 by 4096 with a stride of 4096 in the 4 MiB block, and 4097 by 544,
//   960 by 544 and 512 by 512 swizzled, and 960 by 544 and 961 by 544 tiled
// - depth stencil strides of 0, 16, 31, 32, 48, 64, 961, 992 and 4096, the D16 and DF32 formats,
//   S8 with NULL depth data, DF32_S8 with NULL stencil data, S8D24 with NULL stencil data, and a
//   linear surface
// - sceGxmColorSurfaceSetData with data 2, 4 and 16 bytes in
// Then, since a wrong implementation may read the memory, data where a block was freed, and last
// NULL data and a NULL surface.
//
// On a PS Vita TV on firmware 3.65 both inits work wherever the data is, in a block that isn't
// mapped or mapped with READ only, USER_MAIN_R, a block for code, the stack, past a block's end or
// where a block was freed, and keep the pointers and stride. Color data 1 or 2 bytes in fails with
// 0x805B0003 (INVALID_VALUE), also in sceGxmColorSurfaceSetData, and depth data 1 to 8 bytes in or
// stencil data 1 or 4 bytes off fails with 0x805B0005 (INVALID_ALIGNMENT). Odd color strides give
// INVALID_ALIGNMENT, and a stride, width or height of 0, a width of 4097 and 960 by 544 swizzled
// give INVALID_VALUE, but 512 by 512 swizzled and both tiled sizes work. Depth stencil strides of
// 32, 64, 992 and 4096 work, and 0, 16, 31, 48 and 961 give INVALID_VALUE. NULL color data works,
// as do S8 with NULL depth data and D16, DF32 and S8D24 with NULL stencil data, but DF32_S8 with
// NULL stencil data and NULL depth and stencil data give INVALID_VALUE.
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)
#define BLOCK_SIZE (4 * MIB)
#define STENCIL_OFFSET (2 * MIB)

// vitasdk has a stub for it but no prototype
int sceKernelFreeMemBlockForVM(SceUID uid);

static char *mapped;

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

static char *alloc(SceUID *uid, SceUInt32 type) {
    *uid = sceKernelAllocMemBlock("gxm surface", type, BLOCK_SIZE, NULL);
    void *base = NULL;
    if (*uid >= 0)
        sceKernelGetMemBlockBase(*uid, &base);
    return base;
}

// A color surface init that prints its result, and what the surface keeps if it worked
static void color(const char *what, SceGxmColorSurfaceType type, unsigned int width,
                  unsigned int height, unsigned int stride, void *data) {
    SceGxmColorSurface surface;
    sceClibMemset(&surface, 0, sizeof(surface));
    const int got = sceGxmColorSurfaceInit(&surface, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, type,
                                           SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                           SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, width, height,
                                           stride, data);
    PRINTF(got, "color surface: %s", what);
    if (got != 0)
        return;
    PRINTF(sceGxmColorSurfaceGetData(&surface) == data, "color surface: %s: it keeps the data",
           what);
    PRINTF(sceGxmColorSurfaceGetStrideInPixels(&surface) == stride,
           "color surface: %s: it keeps the stride", what);
}

static void color_linear(const char *what, void *data) {
    color(what, SCE_GXM_COLOR_SURFACE_LINEAR, 960, 544, 960, data);
}

// A depth stencil surface init that prints its result, and what the surface keeps if it worked
static void depth(const char *what, SceGxmDepthStencilFormat format,
                  SceGxmDepthStencilSurfaceType type, unsigned int stride, void *depth_data,
                  void *stencil_data) {
    SceGxmDepthStencilSurface surface;
    sceClibMemset(&surface, 0, sizeof(surface));
    const int got = sceGxmDepthStencilSurfaceInit(&surface, format, type, stride, depth_data,
                                                  stencil_data);
    PRINTF(got, "depth stencil surface: %s", what);
    if (got != 0)
        return;
    PRINTF(surface.depthData == depth_data && surface.stencilData == stencil_data,
           "depth stencil surface: %s: it keeps the data", what);
    PRINTF(sceGxmDepthStencilSurfaceGetStrideInSamples(&surface) == stride,
           "depth stencil surface: %s: it keeps the stride", what);
}

static void depth_tiled(const char *what, char *data) {
    depth(what, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, 960, data,
          data + STENCIL_OFFSET);
}

// Both inits with data at one place
static void both_at(const char *what, char *data) {
    color_linear(what, data);
    depth_tiled(what, data);
}

static void places(void) {
    both_at("the valid surface", mapped);

    SceUID uid;
    char *base = alloc(&uid, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
    both_at("in a block that isn't mapped", base);
    sceKernelFreeMemBlock(uid);

    base = alloc(&uid, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
    sceGxmMapMemory(base, BLOCK_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);
    both_at("in a block mapped with READ only", base);
    sceGxmUnmapMemory(base);
    sceKernelFreeMemBlock(uid);

    base = alloc(&uid, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R);
    both_at("in a USER_MAIN_R block", base);
    sceKernelFreeMemBlock(uid);

    uid = sceKernelAllocMemBlockForVM("gxm surface vm", BLOCK_SIZE);
    void *vm = NULL;
    sceKernelGetMemBlockBase(uid, &vm);
    both_at("in a block for code", vm);
    sceKernelFreeMemBlockForVM(uid);

    // Only the pointer goes in the surfaces, so a small buffer does
    char on_stack[256] __attribute__((aligned(256)));
    color_linear("on the stack", on_stack);
    depth("on the stack", SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
          960, on_stack, on_stack + 128);
}

static void offsets(void) {
    static const SceSize bys[] = { 1, 2, 4, 8, 16, 32, 64 };
    for (unsigned int i = 0; i < sizeof(bys) / sizeof(bys[0]); i++) {
        char what[48];
        sceClibSnprintf(what, sizeof(what), "data %u bytes in", (unsigned)bys[i]);
        both_at(what, mapped + bys[i]);
    }
    static const SceSize stencil_bys[] = { 1, 4, 16 };
    for (unsigned int i = 0; i < sizeof(stencil_bys) / sizeof(stencil_bys[0]); i++) {
        char what[48];
        sceClibSnprintf(what, sizeof(what), "stencil data %u bytes past its place",
                        (unsigned)stencil_bys[i]);
        depth(what, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, 960,
              mapped, mapped + STENCIL_OFFSET + stencil_bys[i]);
    }
    char *const last_page = mapped + BLOCK_SIZE - 4 * KIB;
    color_linear("in the block's last 4 KiB", last_page);
    depth("in the block's last 4 KiB", SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
          SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, 960, last_page, last_page);
}

static void color_shapes(void) {
    static const unsigned int strides[] = { 961, 962, 964, 968, 1024, 959, 0 };
    for (unsigned int i = 0; i < sizeof(strides) / sizeof(strides[0]); i++) {
        char what[32];
        sceClibSnprintf(what, sizeof(what), "stride %u", strides[i]);
        color(what, SCE_GXM_COLOR_SURFACE_LINEAR, 960, 544, strides[i], mapped);
    }
    color("0 by 544", SCE_GXM_COLOR_SURFACE_LINEAR, 0, 544, 960, mapped);
    color("960 by 0", SCE_GXM_COLOR_SURFACE_LINEAR, 960, 0, 960, mapped);
    color("4096 by 4096 in the 4 MiB block", SCE_GXM_COLOR_SURFACE_LINEAR, 4096, 4096, 4096,
          mapped);
    color("4097 by 544", SCE_GXM_COLOR_SURFACE_LINEAR, 4097, 544, 4097 + 1, mapped);
    color("960 by 544 swizzled", SCE_GXM_COLOR_SURFACE_SWIZZLED, 960, 544, 960, mapped);
    color("512 by 512 swizzled", SCE_GXM_COLOR_SURFACE_SWIZZLED, 512, 512, 512, mapped);
    color("960 by 544 tiled", SCE_GXM_COLOR_SURFACE_TILED, 960, 544, 960, mapped);
    color("961 by 544 tiled", SCE_GXM_COLOR_SURFACE_TILED, 961, 544, 962, mapped);

    SceGxmColorSurface surface;
    PRINT("color surface: init one to set its data",
          sceGxmColorSurfaceInit(&surface, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
                                 SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                 SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, 960, 544, 960, mapped));
    static const SceSize bys[] = { 2, 4, 16 };
    for (unsigned int i = 0; i < sizeof(bys) / sizeof(bys[0]); i++) {
        char *const data = mapped + bys[i];
        const int set = sceGxmColorSurfaceSetData(&surface, data);
        PRINTF(set, "color surface: set data %u bytes in", (unsigned)bys[i]);
        if (set == 0)
            PRINTF(sceGxmColorSurfaceGetData(&surface) == data,
                   "color surface: set data %u bytes in: it keeps the data", (unsigned)bys[i]);
    }
}

static void depth_shapes(void) {
    static const unsigned int strides[] = { 0, 16, 31, 32, 48, 64, 961, 992, 4096 };
    for (unsigned int i = 0; i < sizeof(strides) / sizeof(strides[0]); i++) {
        char what[32];
        sceClibSnprintf(what, sizeof(what), "stride %u", strides[i]);
        depth(what, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
              strides[i], mapped, mapped + STENCIL_OFFSET);
    }
    depth("D16", SCE_GXM_DEPTH_STENCIL_FORMAT_D16, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, 960, mapped,
          NULL);
    depth("DF32", SCE_GXM_DEPTH_STENCIL_FORMAT_DF32, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, 960,
          mapped, NULL);
    depth("S8 with NULL depth data", SCE_GXM_DEPTH_STENCIL_FORMAT_S8,
          SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, 960, NULL, mapped + STENCIL_OFFSET);
    depth("DF32_S8 with NULL stencil data", SCE_GXM_DEPTH_STENCIL_FORMAT_DF32_S8,
          SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, 960, mapped, NULL);
    depth("S8D24 with NULL stencil data", SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
          SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, 960, mapped, NULL);
    depth("linear", SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR, 960,
          mapped, mapped + STENCIL_OFFSET);
}

int main(void) {
    PRINT("initialize libgxm", initialize());
    SceUID block;
    mapped = alloc(&block, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
    PRINT("map the block", sceGxmMapMemory(mapped, BLOCK_SIZE, RW));

    places();
    offsets();
    color_shapes();
    depth_shapes();

    // Memory in no block, which a wrong implementation may fault on if it reads it
    SceUID freed;
    char *const gone = alloc(&freed, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
    sceKernelFreeMemBlock(freed);
    both_at("where a block was freed", gone);

    // NULL pointers go last, so an implementation that crashes on them loses nothing else
    color_linear("NULL data", NULL);
    depth("NULL depth and stencil data", SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
          SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, 960, NULL, NULL);
    PRINT("color surface: a NULL surface",
          sceGxmColorSurfaceInit(NULL, SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
                                 SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                 SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, 960, 544, 960, mapped));
    PRINT("depth stencil surface: a NULL surface",
          sceGxmDepthStencilSurfaceInit(NULL, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                        SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, 960, mapped,
                                        mapped + STENCIL_OFFSET));

    sceGxmUnmapMemory(mapped);
    sceKernelFreeMemBlock(block);
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
