// What sceGxmCreateContext takes for its memory. No scene starts, so the GPU never reads the ring
// buffers, and each context that is made is destroyed. The valid parameters are 2 KiB of host
// memory in a static buffer, the VDM, vertex and fragment ring buffers at their default sizes in
// one USER_RW_UNCACHE block mapped with READ, and the fragment USSE ring buffer at its default size
// in a block mapped as fragment USSE memory, with the offset of that map. Each case changes one
// thing from them, on fresh blocks that are unmapped and freed after it. The cases:
// - the valid parameters, then a second context while the first lives
// - each ring buffer in a block that isn't mapped, in a block mapped with WRITE only, in a CDRAM
//   block mapped with READ, with a size of 4 KiB and of its default size less 2 bytes, and with a
//   base 4 bytes in. For the fragment USSE ring buffer, the block that isn't mapped keeps the
//   offset of the valid map, a block mapped with sceGxmMapMemory with offset 0 takes the place of
//   the WRITE only one, a block mapped as vertex USSE memory with that map's offset comes after
//   it, the CDRAM block is mapped as fragment USSE memory, and its offset moves with its base.
// - host memory of 2 KiB less 4 bytes, of size 0, 4 bytes into the buffer, in a USER_RW_UNCACHE
//   block and in a CDRAM block
// - sceGxmSetVisibilityBuffer on a valid context with a base in a block mapped with READ and WRITE,
//   in a block that isn't mapped, 4 and 16 bytes in, and with strides of 4 KiB and 4 bytes and of 0
// - unmapping the ring buffers while a context lives, then destroying it
// Then, since a wrong implementation may crash on them, each ring buffer with size 0, each ring
// buffer and the host memory with a base 2 bytes in, and last NULL parameters, a NULL context
// pointer and a NULL pointer for each memory.
//
// On a PS Vita TV on firmware 3.65 the VDM, vertex and fragment ring buffers work in a block that
// isn't mapped, mapped with WRITE only or in CDRAM. The fragment USSE ring buffer works unmapped
// with the valid map's offset and in CDRAM, but fails with 0x805B0004 (INVALID_POINTER) mapped with
// sceGxmMapMemory or as vertex USSE memory. A size of 4 KiB works for the VDM and fragment USSE
// ring buffers and fails with 0x805B0003 (INVALID_VALUE) for the other two, as do size 0 and host
// memory under 2 KiB. A size or base 2 bytes off fails with 0x805B0005 (INVALID_ALIGNMENT) and a
// base 4 bytes in works, for host memory too. sceGxmSetVisibilityBuffer takes an unmapped block, a
// base 16 bytes in and a stride of 0, and gives INVALID_ALIGNMENT for a base 4 bytes in or a stride
// of 4 KiB and 4 bytes. Unmapping the ring buffers under a live context works, and NULL pointers
// give INVALID_POINTER.
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

#define HOST_SIZE SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE

enum { VDM, VERTEX, FRAGMENT, USSE, RINGS };

static const struct {
    const char *name;
    SceSize offset; // Where it goes in the valid ring buffers' block
    SceSize size;
} rings[RINGS] = {
    { "VDM ring buffer", 0, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE },
    { "vertex ring buffer", MIB, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE },
    { "fragment ring buffer", 256 * KIB, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE },
    { "fragment USSE ring buffer", 0, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE },
};

// Room behind each buffer, in case a call writes past the size it was given
static char host_mem[2][HOST_SIZE + 64] __attribute__((aligned(16)));

// The blocks of a valid set of parameters
typedef struct Setup {
    SceUID rings, usse;
    char *rings_base, *usse_base;
    unsigned int usse_offset;
    SceGxmContextParams params;
} Setup;

// A block that one case uses in place of a valid one
typedef struct Extra {
    SceUID uid;
    char *base;
    int mapped; // 0 for none, 1 for sceGxmMapMemory, 2 for vertex USSE, 3 for fragment USSE
    unsigned int offset;
} Extra;

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

static char *alloc(SceUID *uid, SceUInt32 type, SceSize size) {
    *uid = sceKernelAllocMemBlock("gxm context", type, size, NULL);
    void *base = NULL;
    if (*uid >= 0)
        sceKernelGetMemBlockBase(*uid, &base);
    return base;
}

static void **ring_base(SceGxmContextParams *params, int ring) {
    switch (ring) {
    case VDM:
        return &params->vdmRingBufferMem;
    case VERTEX:
        return &params->vertexRingBufferMem;
    case FRAGMENT:
        return &params->fragmentRingBufferMem;
    default:
        return &params->fragmentUsseRingBufferMem;
    }
}

static SceSize *ring_size(SceGxmContextParams *params, int ring) {
    switch (ring) {
    case VDM:
        return &params->vdmRingBufferMemSize;
    case VERTEX:
        return &params->vertexRingBufferMemSize;
    case FRAGMENT:
        return &params->fragmentRingBufferMemSize;
    default:
        return &params->fragmentUsseRingBufferMemSize;
    }
}

// Makes fresh blocks for a valid set of parameters, with the host memory of the given buffer
static void setup(Setup *s, int host) {
    s->rings_base = alloc(&s->rings, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * MIB);
    sceGxmMapMemory(s->rings_base, 4 * MIB, SCE_GXM_MEMORY_ATTRIB_READ);
    s->usse_base = alloc(&s->usse, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 64 * KIB);
    s->usse_offset = 0;
    sceGxmMapFragmentUsseMemory(s->usse_base, 64 * KIB, &s->usse_offset);

    SceGxmContextParams params = { 0 };
    params.hostMem = host_mem[host];
    params.hostMemSize = HOST_SIZE;
    for (int ring = 0; ring < RINGS; ring++) {
        char *const block = ring == USSE ? s->usse_base : s->rings_base;
        *ring_base(&params, ring) = block + rings[ring].offset;
        *ring_size(&params, ring) = rings[ring].size;
    }
    params.fragmentUsseRingBufferOffset = s->usse_offset;
    s->params = params;
}

static void teardown(Setup *s) {
    sceGxmUnmapFragmentUsseMemory(s->usse_base);
    sceKernelFreeMemBlock(s->usse);
    sceGxmUnmapMemory(s->rings_base);
    sceKernelFreeMemBlock(s->rings);
}

// A fresh block of the type, mapped the given way
static Extra extra(SceUInt32 type, SceSize size, int mapped, SceUInt32 attribs) {
    Extra e = { .mapped = mapped };
    e.base = alloc(&e.uid, type, size);
    if (mapped == 1)
        sceGxmMapMemory(e.base, size, attribs);
    else if (mapped == 2)
        sceGxmMapVertexUsseMemory(e.base, size, &e.offset);
    else if (mapped == 3)
        sceGxmMapFragmentUsseMemory(e.base, size, &e.offset);
    return e;
}

static void extra_free(const Extra *e) {
    if (e->mapped == 1)
        sceGxmUnmapMemory(e->base);
    else if (e->mapped == 2)
        sceGxmUnmapVertexUsseMemory(e->base);
    else if (e->mapped == 3)
        sceGxmUnmapFragmentUsseMemory(e->base);
    sceKernelFreeMemBlock(e->uid);
}

// Makes a context, prints the result and destroys a context that was made, printing that too
static void create_once(const char *label, const SceGxmContextParams *params) {
    SceGxmContext *context = NULL;
    const int created = sceGxmCreateContext(params, &context);
    PRINTF(created, "%s", label);
    if (created == 0)
        PRINTF(sceGxmDestroyContext(context), "%s: destroy it", label);
}

static void valid(void) {
    Setup s;
    setup(&s, 0);
    create_once("the valid parameters", &s.params);

    Setup second;
    setup(&second, 1);
    SceGxmContext *first = NULL, *other = NULL;
    const int created = sceGxmCreateContext(&s.params, &first);
    PRINT("two at once: the first", created);
    const int again = sceGxmCreateContext(&second.params, &other);
    PRINT("two at once: the second", again);
    if (again == 0)
        PRINT("two at once: destroy the second", sceGxmDestroyContext(other));
    if (created == 0)
        PRINT("two at once: destroy the first", sceGxmDestroyContext(first));
    teardown(&second);
    teardown(&s);
}

// Makes a context with one ring buffer in a block of its own, and prints the result
static void ring_in(int ring, const char *what, SceUInt32 type, SceSize size, int mapped,
                    SceUInt32 attribs) {
    Setup s;
    setup(&s, 0);
    const Extra e = extra(type, size, mapped, attribs);
    *ring_base(&s.params, ring) = e.base;
    if (ring == USSE && mapped >= 2)
        s.params.fragmentUsseRingBufferOffset = e.offset;
    else if (ring == USSE && mapped == 1)
        s.params.fragmentUsseRingBufferOffset = 0;
    char label[96];
    sceClibSnprintf(label, sizeof(label), "%s: %s", rings[ring].name, what);
    create_once(label, &s.params);
    extra_free(&e);
    teardown(&s);
}

// Makes a context with one ring buffer's size changed, and prints the result
static void ring_sized(int ring, const char *what, SceSize size) {
    Setup s;
    setup(&s, 0);
    *ring_size(&s.params, ring) = size;
    char label[96];
    sceClibSnprintf(label, sizeof(label), "%s: %s", rings[ring].name, what);
    create_once(label, &s.params);
    teardown(&s);
}

// Makes a context with one ring buffer's base moved by the given bytes, and prints the result
static void ring_moved(int ring, SceSize by) {
    Setup s;
    setup(&s, 0);
    *ring_base(&s.params, ring) = (char *)*ring_base(&s.params, ring) + by;
    if (ring == USSE)
        s.params.fragmentUsseRingBufferOffset += by;
    char label[96];
    sceClibSnprintf(label, sizeof(label), "%s: a base %u bytes in", rings[ring].name,
                    (unsigned)by);
    create_once(label, &s.params);
    teardown(&s);
}

static void ring_cases(int ring) {
    // The fragment USSE ring buffer's blocks are small, the others' are as big as the buffer
    const SceSize size = ring == USSE ? 64 * KIB : rings[ring].size;
    const SceSize cdram = size < 256 * KIB ? 256 * KIB : size;
    if (ring == USSE) {
        ring_in(ring, "in a block that isn't mapped", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                size, 0, 0);
        ring_in(ring, "mapped with sceGxmMapMemory, offset 0",
                SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, 1, RW);
        ring_in(ring, "mapped as vertex USSE memory", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                size, 2, 0);
        ring_in(ring, "in a CDRAM block", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, cdram, 3, 0);
    } else {
        ring_in(ring, "in a block that isn't mapped", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                size, 0, 0);
        ring_in(ring, "in a block mapped with WRITE only",
                SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, 1, SCE_GXM_MEMORY_ATTRIB_WRITE);
        ring_in(ring, "in a CDRAM block", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, cdram, 1,
                SCE_GXM_MEMORY_ATTRIB_READ);
    }
    ring_sized(ring, "size 4 KiB", 4 * KIB);
    ring_sized(ring, "its default size less 2 bytes", rings[ring].size - 2);
    ring_moved(ring, 4);
}

// Makes a context with the host memory changed, and prints the result
static void host_with(const char *what, void *mem, SceSize size) {
    Setup s;
    setup(&s, 0);
    s.params.hostMem = mem;
    s.params.hostMemSize = size;
    char label[96];
    sceClibSnprintf(label, sizeof(label), "host memory: %s", what);
    create_once(label, &s.params);
    teardown(&s);
}

static void host_cases(void) {
    host_with("2 KiB less 4 bytes", host_mem[0], HOST_SIZE - 4);
    host_with("size 0", host_mem[0], 0);
    host_with("4 bytes into the buffer", host_mem[0] + 4, HOST_SIZE);
    SceUID uid;
    char *const uncached = alloc(&uid, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 64 * KIB);
    host_with("in a USER_RW_UNCACHE block", uncached, HOST_SIZE);
    sceKernelFreeMemBlock(uid);
    char *const cdram = alloc(&uid, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB);
    host_with("in a CDRAM block", cdram, HOST_SIZE);
    sceKernelFreeMemBlock(uid);
}

static void visibility(void) {
    Setup s;
    setup(&s, 0);
    SceGxmContext *context = NULL;
    const int created = sceGxmCreateContext(&s.params, &context);
    PRINT("visibility buffer: make a context", created);
    if (created != 0) {
        teardown(&s);
        return;
    }
    const Extra mapped = extra(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 64 * KIB, 1, RW);
    const Extra unmapped = extra(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 64 * KIB, 0, 0);
    PRINT("visibility buffer: in a mapped block",
          sceGxmSetVisibilityBuffer(context, mapped.base, 4 * KIB));
    PRINT("visibility buffer: in a block that isn't mapped",
          sceGxmSetVisibilityBuffer(context, unmapped.base, 4 * KIB));
    PRINT("visibility buffer: a base 4 bytes in",
          sceGxmSetVisibilityBuffer(context, mapped.base + 4, 4 * KIB));
    PRINT("visibility buffer: a base 16 bytes in",
          sceGxmSetVisibilityBuffer(context, mapped.base + 16, 4 * KIB));
    PRINT("visibility buffer: a stride of 4 KiB and 4 bytes",
          sceGxmSetVisibilityBuffer(context, mapped.base, 4 * KIB + 4));
    PRINT("visibility buffer: a stride of 0", sceGxmSetVisibilityBuffer(context, mapped.base, 0));
    PRINT("visibility buffer: destroy the context", sceGxmDestroyContext(context));
    extra_free(&unmapped);
    extra_free(&mapped);
    teardown(&s);
}

static void unmap_while_live(void) {
    Setup s;
    setup(&s, 0);
    SceGxmContext *context = NULL;
    const int created = sceGxmCreateContext(&s.params, &context);
    PRINT("unmap while a context lives: make it", created);
    PRINT("unmap while a context lives: the ring buffers", sceGxmUnmapMemory(s.rings_base));
    PRINT("unmap while a context lives: the fragment USSE ring buffer",
          sceGxmUnmapFragmentUsseMemory(s.usse_base));
    if (created == 0)
        PRINT("unmap while a context lives: destroy it", sceGxmDestroyContext(context));
    // Both are unmapped already, so these unmaps of teardown fail and only the frees matter
    teardown(&s);
}

// Makes a context with one memory set to NULL, and prints the result
static void null_ring(int ring) {
    Setup s;
    setup(&s, 0);
    *ring_base(&s.params, ring) = NULL;
    char label[96];
    sceClibSnprintf(label, sizeof(label), "%s: NULL", rings[ring].name);
    create_once(label, &s.params);
    teardown(&s);
}

int main(void) {
    PRINT("initialize libgxm", initialize());
    valid();
    for (int ring = 0; ring < RINGS; ring++)
        ring_cases(ring);
    host_cases();
    visibility();
    unmap_while_live();

    // Sizes and bases a wrong implementation may fault on when it writes to the ring buffers
    for (int ring = 0; ring < RINGS; ring++)
        ring_sized(ring, "size 0", 0);
    for (int ring = 0; ring < RINGS; ring++)
        ring_moved(ring, 2);
    host_with("2 bytes into the buffer", host_mem[0] + 2, HOST_SIZE);

    // NULL pointers go last, so an implementation that crashes on them loses nothing else. The
    // VDM ring buffer and the host memory come last, since a wrong implementation may use them.
    Setup s;
    setup(&s, 0);
    SceGxmContext *context = NULL;
    PRINT("NULL parameters", sceGxmCreateContext(NULL, &context));
    PRINT("a NULL context pointer", sceGxmCreateContext(&s.params, NULL));
    teardown(&s);
    null_ring(VERTEX);
    null_ring(FRAGMENT);
    null_ring(USSE);
    null_ring(VDM);
    host_with("NULL", NULL, HOST_SIZE);

    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
