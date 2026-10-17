// libgxm's map and unmap calls while it isn't initialized, past the sceGxmMapMemory calls that
// gxm_map_memory makes then. Before sceGxmInitialize: sceGxmUnmapMemory, the USSE maps and
// unmaps, and calls that would fail anyway, with a base that isn't 4 KiB aligned, attributes 0 or
// a USSE size over 8 MiB, to see which check comes first. Then a mapping of each kind left at
// sceGxmTerminate: their unmaps and the USSE maps after it, and after initializing again, their
// unmaps, mapping their ranges again, and whether a USSE range mapped again gets the offset it
// had. Every map that works is unmapped again, and libgxm is terminated at the end.
//
// On a PS Vita TV on firmware 3.65 every call gives 0x805B0000 (UNINITIALIZED) before
// sceGxmInitialize, whatever else is wrong with its arguments, and after sceGxmTerminate, which
// works with the mappings left. Once libgxm is initialized again the old mappings are gone: their
// unmaps give 0x805B0017 (DRIVER), their ranges map again, and each USSE range gets the offset it
// had.
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

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

// Maps, prints the result and unmaps a mapping that worked, printing that too
static void map_once(const char *label, void *base, SceSize size, SceUInt32 attribs) {
    const int mapped = sceGxmMapMemory(base, size, attribs);
    PRINTF(mapped, "%s", label);
    if (mapped == 0)
        PRINTF(sceGxmUnmapMemory(base), "%s: unmap it", label);
}

static void vertex_once(const char *label, void *base, SceSize size) {
    unsigned int offset = 0;
    const int mapped = sceGxmMapVertexUsseMemory(base, size, &offset);
    PRINTF(mapped, "%s", label);
    if (mapped == 0)
        PRINTF(sceGxmUnmapVertexUsseMemory(base), "%s: unmap it", label);
}

static void fragment_once(const char *label, void *base, SceSize size) {
    unsigned int offset = 0;
    const int mapped = sceGxmMapFragmentUsseMemory(base, size, &offset);
    PRINTF(mapped, "%s", label);
    if (mapped == 0)
        PRINTF(sceGxmUnmapFragmentUsseMemory(base), "%s: unmap it", label);
}

static void before_initializing(char *base) {
    PRINT("before initializing: unmap", sceGxmUnmapMemory(base));
    vertex_once("before initializing: vertex USSE 4 KiB", base, 4 * KIB);
    PRINT("before initializing: unmap vertex USSE", sceGxmUnmapVertexUsseMemory(base));
    fragment_once("before initializing: fragment USSE 4 KiB", base, 4 * KIB);
    PRINT("before initializing: unmap fragment USSE", sceGxmUnmapFragmentUsseMemory(base));

    map_once("before initializing: map an unaligned base", base + 2 * KIB, 64 * KIB, RW);
    map_once("before initializing: map with attributes 0", base, 64 * KIB, 0);
    vertex_once("before initializing: vertex USSE 8 MiB and 4 KiB", base, 8 * MIB + 4 * KIB);
    vertex_once("before initializing: vertex USSE with an unaligned base", base + 2 * KIB, 4 * KIB);
}

int main(void) {
    const SceUID block = sceKernelAllocMemBlock("gxm uninitialized",
                                                SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 16 * MIB,
                                                NULL);
    PRINT("allocate a 16 MiB block", test_created(block));
    if (block < 0) {
        test_exit();
        return 0;
    }
    void *block_base = NULL;
    sceKernelGetMemBlockBase(block, &block_base);
    char *const base = block_base;
    // Apart, so each kind has a range of its own
    char *const memory = base, *const vertex = base + MIB, *const fragment = base + 2 * MIB;

    before_initializing(base);

    PRINT("initialize libgxm", initialize());
    unsigned int vertex_offset = 0, fragment_offset = 0, offset = 0;
    const int memory_left = sceGxmMapMemory(memory, 64 * KIB, RW);
    PRINT("map memory to leave mapped", memory_left);
    const int vertex_left = sceGxmMapVertexUsseMemory(vertex, 4 * KIB, &vertex_offset);
    PRINT("map vertex USSE to leave mapped", vertex_left);
    const int fragment_left = sceGxmMapFragmentUsseMemory(fragment, 4 * KIB, &fragment_offset);
    PRINT("map fragment USSE to leave mapped", fragment_left);
    PRINT("terminate libgxm with them mapped", sceGxmTerminate());

    PRINT("after terminating: unmap the memory", sceGxmUnmapMemory(memory));
    PRINT("after terminating: unmap vertex USSE", sceGxmUnmapVertexUsseMemory(vertex));
    PRINT("after terminating: unmap fragment USSE", sceGxmUnmapFragmentUsseMemory(fragment));
    vertex_once("after terminating: vertex USSE 4 KiB", vertex, 4 * KIB);
    fragment_once("after terminating: fragment USSE 4 KiB", fragment, 4 * KIB);

    PRINT("initialize libgxm again", initialize());
    if (memory_left == 0)
        PRINT("after initializing again: unmap the memory", sceGxmUnmapMemory(memory));
    if (vertex_left == 0)
        PRINT("after initializing again: unmap vertex USSE", sceGxmUnmapVertexUsseMemory(vertex));
    if (fragment_left == 0)
        PRINT("after initializing again: unmap fragment USSE",
              sceGxmUnmapFragmentUsseMemory(fragment));
    map_once("after initializing again: map the memory's range", memory, 64 * KIB, RW);

    int mapped = sceGxmMapVertexUsseMemory(vertex, 4 * KIB, &offset);
    PRINT("after initializing again: map the vertex USSE range", mapped);
    PRINT("after initializing again: it gets the vertex USSE offset it had",
          mapped == 0 && offset == vertex_offset);
    if (mapped == 0)
        PRINT("after initializing again: unmap vertex USSE", sceGxmUnmapVertexUsseMemory(vertex));
    mapped = sceGxmMapFragmentUsseMemory(fragment, 4 * KIB, &offset);
    PRINT("after initializing again: map the fragment USSE range", mapped);
    PRINT("after initializing again: it gets the fragment USSE offset it had",
          mapped == 0 && offset == fragment_offset);
    if (mapped == 0)
        PRINT("after initializing again: unmap fragment USSE",
              sceGxmUnmapFragmentUsseMemory(fragment));

    PRINT("terminate libgxm again", sceGxmTerminate());
    sceKernelFreeMemBlock(block);
    test_exit();
    return 0;
}
