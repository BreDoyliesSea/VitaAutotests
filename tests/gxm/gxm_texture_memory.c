// What the sceGxmTextureInit calls take for the memory a texture describes, and whether the texture
// keeps the pointers it was given. No scene starts, so the GPU never reads the memory. The valid
// texture is 256 by 256 U8U8U8U8_ABGR with 1 mip level, or a stride of 1024 bytes for a strided
// one, at the base of a 4 MiB USER_RW_UNCACHE block mapped with READ unless the case says
// otherwise. Each init that works prints whether sceGxmTextureGetData gives back the data pointer,
// and what sceGxmTextureValidate gives. The cases:
// - the valid texture of each kind, then a linear texture with its data in a block that isn't
//   mapped, in a USER_MAIN_R block, in a block for code and on the stack
// - a linear, strided, swizzled, tiled and cube texture each with data 1, 2, 4, 8, 16, 32 and 64
//   bytes in
// - linear textures with data in the block's last 4 KiB, of 4096 by 4096 in the 4 MiB block, of 0
//   by 256, 256 by 0, 1 by 1, 3 by 3, 4097 by 256 and 256 by 4097, and with 0, 9, 10, 13, 14 and 16
//   mip levels
// - strided textures with byte strides of 0, 2, 4, 6, 1028, 512, which is under a row's 1024 bytes,
//   131072 and 131076, which also print whether sceGxmTextureGetStride gives back the stride
// - swizzled textures of 255 by 256 and 4096 by 4096, tiled textures of 33 by 32 and 1 by 1, and a
//   cube texture of 255 by 256
// - sceGxmTextureSetData with data 2, 4 and 16 bytes in, and on a P8 texture,
//   sceGxmTextureSetPalette with a palette at the block's base, 16, 32 and 64 bytes in and in a
//   block that isn't mapped, with whether sceGxmTextureGetPalette gives it back
// Then, since a wrong implementation may read the memory, data and a palette where a block was
// freed, and last NULL data, a NULL palette and a NULL texture.
//
// On a PS Vita TV on firmware 3.65 every kind of texture is made wherever the data is, in a block
// that isn't mapped, USER_MAIN_R, a block for code, the stack, past a block's end or where a block
// was freed, and keeps its pointers and stride and validates. Data 1 or 2 bytes in fails with
// 0x805B0005 (INVALID_ALIGNMENT), also in sceGxmTextureSetData. A width or height of 0 or 4097, 14
// or 16 mip levels and a tiled 1 by 1 texture give 0x805B0003 (INVALID_VALUE), while 0, 9, 10 and
// 13 mip levels work. Strides of 4, 512, 1028 and 131072 bytes work, 2 and 6 give
// INVALID_ALIGNMENT, and 0 and 131076 INVALID_VALUE. Swizzled and cube textures of 255 by 256 give
// INVALID_ALIGNMENT. A palette works at the block's base or 64 bytes in, also in a block that isn't
// mapped, and gives INVALID_ALIGNMENT 16 or 32 bytes in. NULL data makes a texture that fails
// validation with 0x805B0019 (INVALID_TEXTURE_DATA_POINTER).
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define BLOCK_SIZE (4 * MIB)
#define FORMAT SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR

// vitasdk has a stub for it but no prototype
int sceKernelFreeMemBlockForVM(SceUID uid);

typedef int (*Init)(SceGxmTexture *texture, const void *data, SceGxmTextureFormat format,
                    unsigned int width, unsigned int height, unsigned int last);

// One init call, with the last argument of a valid texture: its mip count or its stride
typedef struct Kind {
    const char *name;
    Init init;
    unsigned int last;
} Kind;

enum { LINEAR, STRIDED, SWIZZLED, TILED, CUBE, KINDS };

static const Kind kinds[KINDS] = {
    { "linear", sceGxmTextureInitLinear, 1 },
    { "strided", sceGxmTextureInitLinearStrided, 1024 },
    { "swizzled", sceGxmTextureInitSwizzled, 1 },
    { "tiled", sceGxmTextureInitTiled, 1 },
    { "cube", sceGxmTextureInitCube, 1 },
};

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
    *uid = sceKernelAllocMemBlock("gxm texture", type, BLOCK_SIZE, NULL);
    void *base = NULL;
    if (*uid >= 0)
        sceKernelGetMemBlockBase(*uid, &base);
    return base;
}

// An init that prints its result, and if it worked what the texture keeps and its validation
static void init(int kind, const char *what, const void *data, unsigned int width,
                 unsigned int height, unsigned int last) {
    SceGxmTexture texture;
    sceClibMemset(&texture, 0, sizeof(texture));
    const int got = kinds[kind].init(&texture, data, FORMAT, width, height, last);
    PRINTF(got, "%s texture: %s", kinds[kind].name, what);
    if (got != 0)
        return;
    PRINTF(sceGxmTextureGetData(&texture) == data, "%s texture: %s: it keeps the data",
           kinds[kind].name, what);
    if (kind == STRIDED)
        PRINTF(sceGxmTextureGetStride(&texture) == last, "%s texture: %s: it keeps the stride",
               kinds[kind].name, what);
    PRINTF(sceGxmTextureValidate(&texture), "%s texture: %s: validate it", kinds[kind].name,
           what);
}

static void valid(int kind, const char *what, const void *data) {
    init(kind, what, data, 256, 256, kinds[kind].last);
}

static void places(void) {
    for (int kind = 0; kind < KINDS; kind++)
        valid(kind, "the valid texture", mapped);

    SceUID uid;
    char *base = alloc(&uid, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
    valid(LINEAR, "in a block that isn't mapped", base);
    sceKernelFreeMemBlock(uid);

    base = alloc(&uid, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R);
    valid(LINEAR, "in a USER_MAIN_R block", base);
    sceKernelFreeMemBlock(uid);

    uid = sceKernelAllocMemBlockForVM("gxm texture vm", BLOCK_SIZE);
    void *vm = NULL;
    sceKernelGetMemBlockBase(uid, &vm);
    valid(LINEAR, "in a block for code", vm);
    sceKernelFreeMemBlockForVM(uid);

    // Only the pointer goes in the texture, so a small buffer does
    char on_stack[256] __attribute__((aligned(256)));
    valid(LINEAR, "on the stack", on_stack);
}

static void offsets(void) {
    static const SceSize bys[] = { 1, 2, 4, 8, 16, 32, 64 };
    for (int kind = 0; kind < KINDS; kind++) {
        for (unsigned int i = 0; i < sizeof(bys) / sizeof(bys[0]); i++) {
            char what[32];
            sceClibSnprintf(what, sizeof(what), "data %u bytes in", (unsigned)bys[i]);
            valid(kind, what, mapped + bys[i]);
        }
    }
}

static void shapes(void) {
    valid(LINEAR, "in the block's last 4 KiB", mapped + BLOCK_SIZE - 4 * KIB);
    init(LINEAR, "4096 by 4096 in the 4 MiB block", mapped, 4096, 4096, 1);
    static const struct {
        unsigned int width, height;
    } dims[] = { { 0, 256 }, { 256, 0 }, { 1, 1 }, { 3, 3 }, { 4097, 256 }, { 256, 4097 } };
    for (unsigned int i = 0; i < sizeof(dims) / sizeof(dims[0]); i++) {
        char what[32];
        sceClibSnprintf(what, sizeof(what), "%u by %u", dims[i].width, dims[i].height);
        init(LINEAR, what, mapped, dims[i].width, dims[i].height, 1);
    }
    static const unsigned int mips[] = { 0, 9, 10, 13, 14, 16 };
    for (unsigned int i = 0; i < sizeof(mips) / sizeof(mips[0]); i++) {
        char what[32];
        sceClibSnprintf(what, sizeof(what), "%u mip levels", mips[i]);
        init(LINEAR, what, mapped, 256, 256, mips[i]);
    }

    static const unsigned int strides[] = { 0, 2, 4, 6, 1028, 512, 131072, 131076 };
    for (unsigned int i = 0; i < sizeof(strides) / sizeof(strides[0]); i++) {
        char what[32];
        sceClibSnprintf(what, sizeof(what), "a stride of %u bytes", strides[i]);
        init(STRIDED, what, mapped, 256, 256, strides[i]);
    }

    init(SWIZZLED, "255 by 256", mapped, 255, 256, 1);
    init(SWIZZLED, "4096 by 4096", mapped, 4096, 4096, 1);
    init(TILED, "33 by 32", mapped, 33, 32, 1);
    init(TILED, "1 by 1", mapped, 1, 1, 1);
    init(CUBE, "255 by 256", mapped, 255, 256, 1);
}

// Sets the data of a valid linear texture, and prints the result and whether it keeps it
static void set_data(const char *what, const void *data) {
    SceGxmTexture texture;
    sceClibMemset(&texture, 0, sizeof(texture));
    sceGxmTextureInitLinear(&texture, mapped, FORMAT, 256, 256, 1);
    const int set = sceGxmTextureSetData(&texture, data);
    PRINTF(set, "set data %s", what);
    if (set == 0)
        PRINTF(sceGxmTextureGetData(&texture) == data, "set data %s: it keeps it", what);
}

// Sets the palette of a valid P8 texture, and prints the result and whether it keeps it
static void set_palette(const char *what, const void *palette) {
    SceGxmTexture texture;
    sceClibMemset(&texture, 0, sizeof(texture));
    sceGxmTextureInitLinear(&texture, mapped, SCE_GXM_TEXTURE_FORMAT_P8_ABGR, 256, 256, 1);
    const int set = sceGxmTextureSetPalette(&texture, palette);
    PRINTF(set, "set a palette %s", what);
    if (set == 0)
        PRINTF(sceGxmTextureGetPalette(&texture) == palette, "set a palette %s: it keeps it",
               what);
}

static void setters(void) {
    set_data("2 bytes in", mapped + 2);
    set_data("4 bytes in", mapped + 4);
    set_data("16 bytes in", mapped + 16);

    SceGxmTexture texture;
    PRINT("init a P8 texture",
          sceGxmTextureInitLinear(&texture, mapped, SCE_GXM_TEXTURE_FORMAT_P8_ABGR, 256, 256, 1));
    set_palette("at the block's base", mapped);
    set_palette("16 bytes in", mapped + 16);
    set_palette("32 bytes in", mapped + 32);
    set_palette("64 bytes in", mapped + 64);
    SceUID uid;
    char *const base = alloc(&uid, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
    set_palette("in a block that isn't mapped", base);
    sceKernelFreeMemBlock(uid);
}

int main(void) {
    PRINT("initialize libgxm", initialize());
    SceUID block;
    mapped = alloc(&block, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
    PRINT("map the block", sceGxmMapMemory(mapped, BLOCK_SIZE, SCE_GXM_MEMORY_ATTRIB_READ));

    places();
    offsets();
    shapes();
    setters();

    // Memory in no block, which a wrong implementation may fault on if it reads it
    SceUID freed;
    char *const gone = alloc(&freed, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE);
    sceKernelFreeMemBlock(freed);
    valid(LINEAR, "where a block was freed", gone);
    set_palette("where a block was freed", gone);

    // NULL pointers go last, so an implementation that crashes on them loses nothing else
    valid(LINEAR, "NULL data", NULL);
    set_palette("NULL", NULL);
    PRINT("linear texture: a NULL texture",
          sceGxmTextureInitLinear(NULL, mapped, FORMAT, 256, 256, 1));

    sceGxmUnmapMemory(mapped);
    sceKernelFreeMemBlock(block);
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
