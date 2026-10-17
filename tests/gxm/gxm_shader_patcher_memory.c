// What sceGxmShaderPatcherCreate takes for the GPU memory of a shader patcher: allocation
// callbacks or static regions for its buffers, vertex USSE code and fragment USSE code. No program
// is made, so nothing is ever written to the regions for the GPU. Host memory always comes from
// malloc through the host callbacks. The buffer and USSE callbacks return NULL, and the first two
// cases print how often the create called them. A static region is a fresh 64 KiB USER_RW_UNCACHE
// block, mapped the way the case says and unmapped and freed after it. Each patcher that is made
// is destroyed. The cases:
// - a create before sceGxmInitialize, with callbacks for everything
// - callbacks for everything, and all three static regions mapped the way they should be
// - a static buffer region mapped with READ and WRITE, in a block that isn't mapped, mapped with
//   READ only, of size 0, of 64 KiB less 2 bytes, 2, 4 and 16 bytes in, and with buffer callbacks
//   too. Then neither a buffer region nor buffer callbacks.
// - static vertex USSE and fragment USSE regions each mapped as USSE code of their kind, in a block
//   that isn't mapped with offset 0, mapped as the other kind with that map's offset, mapped with
//   sceGxmMapMemory with offset 0, and 4 and 16 bytes in, with the offset moved as far
// - one shared USSE region for both kinds, mapped as both, and mapped only as vertex USSE code
// Then, since a wrong implementation may crash on them, a host allocation callback that returns
// NULL, and last NULL parameters, a NULL patcher pointer and a NULL host allocation callback.
//
// On a PS Vita TV on firmware 3.65 a patcher is made before sceGxmInitialize too, and neither
// create with callbacks calls a buffer or USSE callback. A static buffer region works mapped with
// READ and WRITE or READ only, in a block that isn't mapped, of 64 KiB less 2 bytes and 2, 4 or 16
// bytes in. It fails with 0x805B0003 (INVALID_VALUE) of size 0 or together with buffer callbacks,
// and so does having neither. A static USSE region of either kind works mapped as its kind, as the
// other kind, with sceGxmMapMemory or not at all, and 4 or 16 bytes in, and one region for both
// kinds works mapped as both or only as vertex USSE code. A host allocation callback that returns
// NULL fails with 0x805B0021, and NULL parameters, a NULL patcher pointer and a NULL host
// allocation callback give 0x805B0004 (INVALID_POINTER).
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <stdlib.h>

#include "test.h"

#define KIB 0x400

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)
#define REGION_SIZE (64 * KIB)

// How a static region is mapped
enum { NONE, MEMORY_RW, MEMORY_R, VERTEX, FRAGMENT, BOTH };

typedef struct Region {
    SceUID uid;
    char *base;
    int how;
    unsigned int vertex_offset, fragment_offset;
} Region;

static volatile int buffer_allocs, usse_allocs;

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

static void *host_alloc(void *user_data, SceSize size) {
    (void)user_data;
    return malloc(size);
}

static void *host_alloc_none(void *user_data, SceSize size) {
    (void)user_data;
    (void)size;
    return NULL;
}

static void host_free(void *user_data, void *mem) {
    (void)user_data;
    free(mem);
}

static void *buffer_alloc(void *user_data, SceSize size) {
    (void)user_data;
    (void)size;
    buffer_allocs++;
    return NULL;
}

static void *usse_alloc(void *user_data, SceSize size, unsigned int *usse_offset) {
    (void)user_data;
    (void)size;
    usse_allocs++;
    *usse_offset = 0;
    return NULL;
}

static void mem_free(void *user_data, void *mem) {
    (void)user_data;
    (void)mem;
}

static SceGxmShaderPatcherParams with_callbacks(void) {
    SceGxmShaderPatcherParams params = { 0 };
    params.hostAllocCallback = host_alloc;
    params.hostFreeCallback = host_free;
    params.bufferAllocCallback = buffer_alloc;
    params.bufferFreeCallback = mem_free;
    params.vertexUsseAllocCallback = usse_alloc;
    params.vertexUsseFreeCallback = mem_free;
    params.fragmentUsseAllocCallback = usse_alloc;
    params.fragmentUsseFreeCallback = mem_free;
    return params;
}

static Region region(int how) {
    Region r = { .how = how };
    r.uid = sceKernelAllocMemBlock("gxm patcher", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                                   REGION_SIZE, NULL);
    void *base = NULL;
    sceKernelGetMemBlockBase(r.uid, &base);
    r.base = base;
    if (how == MEMORY_RW)
        sceGxmMapMemory(r.base, REGION_SIZE, RW);
    else if (how == MEMORY_R)
        sceGxmMapMemory(r.base, REGION_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);
    if (how == VERTEX || how == BOTH)
        sceGxmMapVertexUsseMemory(r.base, REGION_SIZE, &r.vertex_offset);
    if (how == FRAGMENT || how == BOTH)
        sceGxmMapFragmentUsseMemory(r.base, REGION_SIZE, &r.fragment_offset);
    return r;
}

static void region_free(const Region *r) {
    if (r->how == MEMORY_RW || r->how == MEMORY_R)
        sceGxmUnmapMemory(r->base);
    if (r->how == VERTEX || r->how == BOTH)
        sceGxmUnmapVertexUsseMemory(r->base);
    if (r->how == FRAGMENT || r->how == BOTH)
        sceGxmUnmapFragmentUsseMemory(r->base);
    sceKernelFreeMemBlock(r->uid);
}

// Makes a patcher, prints the result and destroys one that was made, printing that too
static void create_once(const char *what, const SceGxmShaderPatcherParams *params) {
    SceGxmShaderPatcher *patcher = NULL;
    const int created = sceGxmShaderPatcherCreate(params, &patcher);
    PRINTF(created, "%s", what);
    if (created == 0)
        PRINTF(sceGxmShaderPatcherDestroy(patcher), "%s: destroy it", what);
}

// Like create_once, also printing how often the create called the buffer and USSE callbacks
static void create_counting(const char *what, const SceGxmShaderPatcherParams *params) {
    buffer_allocs = usse_allocs = 0;
    SceGxmShaderPatcher *patcher = NULL;
    const int created = sceGxmShaderPatcherCreate(params, &patcher);
    const int buffers = buffer_allocs, usses = usse_allocs;
    PRINTF(created, "%s", what);
    PRINTF(buffers, "%s: buffer allocations", what);
    PRINTF(usses, "%s: USSE allocations", what);
    if (created == 0)
        PRINTF(sceGxmShaderPatcherDestroy(patcher), "%s: destroy it", what);
}

static void all_static(void) {
    const Region buffer = region(MEMORY_RW), vertex = region(VERTEX), fragment = region(FRAGMENT);
    SceGxmShaderPatcherParams params = { 0 };
    params.hostAllocCallback = host_alloc;
    params.hostFreeCallback = host_free;
    params.bufferMem = buffer.base;
    params.bufferMemSize = REGION_SIZE;
    params.vertexUsseMem = vertex.base;
    params.vertexUsseMemSize = REGION_SIZE;
    params.vertexUsseOffset = vertex.vertex_offset;
    params.fragmentUsseMem = fragment.base;
    params.fragmentUsseMemSize = REGION_SIZE;
    params.fragmentUsseOffset = fragment.fragment_offset;
    create_once("all three static regions", &params);
    region_free(&fragment);
    region_free(&vertex);
    region_free(&buffer);
}

// Makes a patcher with a static buffer region, starting the given bytes into a block mapped the
// given way, with the size given
static void buffer_case(const char *what, int how, SceSize by, SceSize size, int callbacks) {
    const Region r = region(how);
    SceGxmShaderPatcherParams params = with_callbacks();
    if (!callbacks) {
        params.bufferAllocCallback = NULL;
        params.bufferFreeCallback = NULL;
    }
    params.bufferMem = r.base + by;
    params.bufferMemSize = size;
    char label[96];
    sceClibSnprintf(label, sizeof(label), "a static buffer region %s", what);
    create_once(label, &params);
    region_free(&r);
}

static void buffer_cases(void) {
    buffer_case("mapped with READ and WRITE", MEMORY_RW, 0, REGION_SIZE, 0);
    buffer_case("in a block that isn't mapped", NONE, 0, REGION_SIZE, 0);
    buffer_case("mapped with READ only", MEMORY_R, 0, REGION_SIZE, 0);
    buffer_case("of size 0", MEMORY_RW, 0, 0, 0);
    buffer_case("of 64 KiB less 2 bytes", MEMORY_RW, 0, REGION_SIZE - 2, 0);
    buffer_case("2 bytes in", MEMORY_RW, 2, REGION_SIZE - 16, 0);
    buffer_case("4 bytes in", MEMORY_RW, 4, REGION_SIZE - 16, 0);
    buffer_case("16 bytes in", MEMORY_RW, 16, REGION_SIZE - 16, 0);
    buffer_case("and buffer callbacks", MEMORY_RW, 0, REGION_SIZE, 1);

    SceGxmShaderPatcherParams params = with_callbacks();
    params.bufferAllocCallback = NULL;
    params.bufferFreeCallback = NULL;
    create_once("no buffer region and no buffer callbacks", &params);
}

// Makes a patcher with a static USSE region of one kind
static void usse_case(int fragment, const char *what, int how, SceSize by, int offset_from) {
    const Region r = region(how);
    SceGxmShaderPatcherParams params = with_callbacks();
    const unsigned int offset = (offset_from == VERTEX     ? r.vertex_offset
                                 : offset_from == FRAGMENT ? r.fragment_offset
                                                           : 0) +
                                by;
    if (fragment) {
        params.fragmentUsseAllocCallback = NULL;
        params.fragmentUsseFreeCallback = NULL;
        params.fragmentUsseMem = r.base + by;
        params.fragmentUsseMemSize = REGION_SIZE - 16;
        params.fragmentUsseOffset = offset;
    } else {
        params.vertexUsseAllocCallback = NULL;
        params.vertexUsseFreeCallback = NULL;
        params.vertexUsseMem = r.base + by;
        params.vertexUsseMemSize = REGION_SIZE - 16;
        params.vertexUsseOffset = offset;
    }
    char label[96];
    sceClibSnprintf(label, sizeof(label), "a static %s USSE region %s",
                    fragment ? "fragment" : "vertex", what);
    create_once(label, &params);
    region_free(&r);
}

static void usse_cases(int fragment) {
    const int own = fragment ? FRAGMENT : VERTEX, other = fragment ? VERTEX : FRAGMENT;
    usse_case(fragment, "mapped as its kind", own, 0, own);
    usse_case(fragment, "in a block that isn't mapped", NONE, 0, NONE);
    usse_case(fragment, "mapped as the other kind", other, 0, other);
    usse_case(fragment, "mapped with sceGxmMapMemory", MEMORY_RW, 0, NONE);
    usse_case(fragment, "4 bytes in", own, 4, own);
    usse_case(fragment, "16 bytes in", own, 16, own);
}

// Makes a patcher whose two USSE regions are one, mapped the given way
static void shared_case(const char *what, int how) {
    const Region r = region(how);
    SceGxmShaderPatcherParams params = with_callbacks();
    params.vertexUsseAllocCallback = NULL;
    params.vertexUsseFreeCallback = NULL;
    params.fragmentUsseAllocCallback = NULL;
    params.fragmentUsseFreeCallback = NULL;
    params.vertexUsseMem = params.fragmentUsseMem = r.base;
    params.vertexUsseMemSize = params.fragmentUsseMemSize = REGION_SIZE;
    params.vertexUsseOffset = r.vertex_offset;
    params.fragmentUsseOffset = r.fragment_offset;
    char label[96];
    sceClibSnprintf(label, sizeof(label), "a shared USSE region %s", what);
    create_once(label, &params);
    region_free(&r);
}

int main(void) {
    SceGxmShaderPatcherParams params = with_callbacks();
    create_counting("before initializing", &params);
    PRINT("initialize libgxm", initialize());

    create_counting("callbacks for everything", &params);
    all_static();
    buffer_cases();
    usse_cases(0);
    usse_cases(1);
    shared_case("mapped as both kinds", BOTH);
    shared_case("mapped only as vertex USSE code", VERTEX);

    // A wrong implementation may use the NULL it gets as memory
    params = with_callbacks();
    params.hostAllocCallback = host_alloc_none;
    create_once("a host allocation callback that returns NULL", &params);

    // NULL pointers go last, so an implementation that crashes on them loses nothing else
    params = with_callbacks();
    SceGxmShaderPatcher *patcher = NULL;
    PRINT("NULL parameters", sceGxmShaderPatcherCreate(NULL, &patcher));
    PRINT("a NULL patcher pointer", sceGxmShaderPatcherCreate(&params, NULL));
    params.hostAllocCallback = NULL;
    create_once("a NULL host allocation callback", &params);

    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
