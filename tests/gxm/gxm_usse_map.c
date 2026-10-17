// What sceGxmMapVertexUsseMemory and sceGxmMapFragmentUsseMemory take past gxm_map_memory, and
// what their unmaps take. No GPU work runs. For each kind:
// - sizes 0, 6 KiB and 1 MiB, an 8 MiB fragment map, and a base that isn't 4 KiB aligned for the
//   fragment kind
// - the same range twice, and 64 KiB that overlaps another 64 KiB mapping by half
// - an 8 MiB map and then another 4 KiB, and two 4 MiB maps and then another 4 KiB, to see whether
//   8 MiB is the limit of one map or of all of them
// - a whole USER_MAIN_R block, a USER_CDRAM_RW block and a block for code
// - unmaps given 4 KiB into a mapping or 1 byte past its base, its base given to the other kind's
//   unmap and to sceGxmUnmapMemory, and the base of a sceGxmMapMemory mapping given to the unmap
// Then both kinds over one range at once, sceGxmMapMemory over a vertex USSE mapping, and for each
// kind how the offsets of two maps in a row relate, and whether a range mapped again after its
// unmap gets the offset it had. Every map that works is unmapped again. Last, a NULL base for each
// call.
//
// On a PS Vita TV on firmware 3.65 both USSE maps fail with 0x805B0017 (DRIVER) for size 0, a size
// that isn't a multiple of 4 KiB, an unaligned base, a range that overlaps a mapping of their own
// kind, and a block for code. They take 8 MiB and then 4 KiB more, or 4 MiB twice and then 4 KiB
// more, so 8 MiB isn't a limit on the total, and USER_MAIN_R and USER_CDRAM_RW blocks. A vertex and
// a fragment map may hold the same range, and so may a USSE map and sceGxmMapMemory. Each unmap
// takes only the base of a mapping of its own kind, and a NULL base gives 0x805B0004
// (INVALID_POINTER). Two maps in a row get offsets 4 KiB apart, not as far apart as their bases,
// and a range mapped again after its unmap gets another offset.
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

// vitasdk has a stub for it but no prototype
int sceKernelFreeMemBlockForVM(SceUID uid);

// The calls of one USSE kind
typedef struct Kind {
    const char *name;
    int (*map)(void *base, SceSize size, unsigned int *offset);
    int (*unmap)(void *base);
} Kind;

static const Kind vertex = { "vertex", sceGxmMapVertexUsseMemory, sceGxmUnmapVertexUsseMemory };
static const Kind fragment = { "fragment", sceGxmMapFragmentUsseMemory,
                               sceGxmUnmapFragmentUsseMemory };

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
    *uid = sceKernelAllocMemBlock("gxm usse", type, size, NULL);
    void *base = NULL;
    if (*uid >= 0)
        sceKernelGetMemBlockBase(*uid, &base);
    return base;
}

// Maps, prints the result and unmaps a mapping that worked, printing that too
static void map_once(const Kind *kind, const char *label, void *base, SceSize size) {
    unsigned int offset = 0;
    const int mapped = kind->map(base, size, &offset);
    PRINTF(mapped, "%s USSE: %s", kind->name, label);
    if (mapped == 0)
        PRINTF(kind->unmap(base), "%s USSE: %s: unmap it", kind->name, label);
}

// Maps first and then second, prints both, and unmaps both
static void map_two(const Kind *kind, const char *label, char *first, SceSize first_size,
                    char *second, SceSize second_size) {
    unsigned int offset = 0;
    const int a = kind->map(first, first_size, &offset);
    PRINTF(a, "%s USSE: %s: the first", kind->name, label);
    const int b = kind->map(second, second_size, &offset);
    PRINTF(b, "%s USSE: %s: the second", kind->name, label);
    if (b == 0)
        PRINTF(kind->unmap(second), "%s USSE: %s: unmap the second", kind->name, label);
    if (a == 0)
        PRINTF(kind->unmap(first), "%s USSE: %s: unmap the first", kind->name, label);
}

static void sizes_and_ranges(const Kind *kind, char *base) {
    map_once(kind, "size 0", base, 0);
    map_once(kind, "6 KiB", base, 6 * KIB);
    map_once(kind, "1 MiB", base, MIB);
    if (kind == &fragment) {
        map_once(kind, "8 MiB", base, 8 * MIB);
        map_once(kind, "an unaligned base", base + 2 * KIB, 4 * KIB);
    }
    map_two(kind, "the same 4 KiB twice", base, 4 * KIB, base, 4 * KIB);
    map_two(kind, "64 KiB overlapping 64 KiB by half", base, 64 * KIB, base + 32 * KIB, 64 * KIB);
    map_two(kind, "8 MiB, then another 4 KiB", base, 8 * MIB, base + 8 * MIB, 4 * KIB);

    unsigned int offset = 0;
    const int a = kind->map(base, 4 * MIB, &offset);
    const int b = kind->map(base + 4 * MIB, 4 * MIB, &offset);
    PRINTF(a, "%s USSE: 4 MiB", kind->name);
    PRINTF(b, "%s USSE: another 4 MiB after it", kind->name);
    map_once(kind, "4 KiB more after the two", base + 8 * MIB, 4 * KIB);
    if (b == 0)
        kind->unmap(base + 4 * MIB);
    if (a == 0)
        kind->unmap(base);
}

static void block_types(const Kind *kind) {
    SceUID uid;
    char *base = alloc(&uid, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R, 64 * KIB);
    if (uid >= 0) {
        map_once(kind, "a whole USER_MAIN_R block", base, 64 * KIB);
        sceKernelFreeMemBlock(uid);
    }
    base = alloc(&uid, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB);
    if (uid >= 0) {
        map_once(kind, "a whole USER_CDRAM_RW block", base, 256 * KIB);
        sceKernelFreeMemBlock(uid);
    }
    uid = sceKernelAllocMemBlockForVM("gxm usse vm", MIB);
    if (uid >= 0) {
        void *vm = NULL;
        sceKernelGetMemBlockBase(uid, &vm);
        map_once(kind, "a whole block for code", vm, MIB);
        sceKernelFreeMemBlockForVM(uid);
    }
}

// Unmaps of a mapping of the kind given the wrong base
static void wrong_unmaps(const Kind *kind, const Kind *other, char *base) {
    unsigned int offset = 0;
    const int mapped = kind->map(base, 64 * KIB, &offset);
    PRINTF(mapped, "%s USSE: map 64 KiB to unmap", kind->name);
    PRINTF(kind->unmap(base + 4 * KIB), "%s USSE: unmap 4 KiB into it", kind->name);
    PRINTF(kind->unmap(base + 1), "%s USSE: unmap 1 byte past its base", kind->name);
    PRINTF(other->unmap(base), "%s USSE: unmap its base as %s USSE", kind->name, other->name);
    PRINTF(sceGxmUnmapMemory(base), "%s USSE: unmap its base with sceGxmUnmapMemory", kind->name);
    if (mapped == 0)
        PRINTF(kind->unmap(base), "%s USSE: unmap its base", kind->name);

    const int memory = sceGxmMapMemory(base, 64 * KIB, RW);
    PRINTF(memory, "%s USSE: map 64 KiB with sceGxmMapMemory", kind->name);
    PRINTF(kind->unmap(base), "%s USSE: unmap that as %s USSE", kind->name, kind->name);
    if (memory == 0)
        sceGxmUnmapMemory(base);
}

static void mixed(char *base) {
    unsigned int v = 0, f = 0;
    const int a = sceGxmMapVertexUsseMemory(base, 4 * KIB, &v);
    const int b = sceGxmMapFragmentUsseMemory(base, 4 * KIB, &f);
    PRINT("vertex USSE, then fragment USSE over the same range: the vertex map", a);
    PRINT("vertex USSE, then fragment USSE over the same range: the fragment map", b);
    if (b == 0)
        PRINT("unmap the fragment USSE mapping", sceGxmUnmapFragmentUsseMemory(base));
    if (a == 0)
        PRINT("unmap the vertex USSE mapping", sceGxmUnmapVertexUsseMemory(base));

    const int usse = sceGxmMapVertexUsseMemory(base, 4 * KIB, &v);
    PRINT("vertex USSE, then sceGxmMapMemory over it: the USSE map", usse);
    const int memory = sceGxmMapMemory(base, MIB, RW);
    PRINT("vertex USSE, then sceGxmMapMemory over it: the memory map", memory);
    if (memory == 0)
        PRINT("unmap the memory mapping", sceGxmUnmapMemory(base));
    if (usse == 0)
        PRINT("unmap the USSE mapping after it", sceGxmUnmapVertexUsseMemory(base));
}

static void offsets(const Kind *kind, char *base) {
    unsigned int first = 0, second = 0, again = 0;
    const int a = kind->map(base, 4 * KIB, &first);
    const int b = kind->map(base + 64 * KIB, 4 * KIB, &second);
    PRINTF(a == 0 && b == 0, "%s USSE offsets: two maps in a row work", kind->name);
    PRINTF(first % (4 * KIB) == 0 && second % (4 * KIB) == 0,
           "%s USSE offsets: both are 4 KiB aligned", kind->name);
    PRINTF(second == first + 4 * KIB, "%s USSE offsets: the second is 4 KiB past the first",
           kind->name);
    PRINTF(second == first + 64 * KIB, "%s USSE offsets: they're as far apart as the bases",
           kind->name);
    if (a == 0)
        kind->unmap(base);
    const int c = kind->map(base, 4 * KIB, &again);
    PRINTF(c == 0 && again == first,
           "%s USSE offsets: mapping the first again after its unmap gives its offset", kind->name);
    if (c == 0)
        kind->unmap(base);
    if (b == 0)
        kind->unmap(base + 64 * KIB);
}

int main(void) {
    PRINT("initialize libgxm", initialize());
    SceUID block;
    char *const base = alloc(&block, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 16 * MIB);
    PRINT("allocate a 16 MiB block", test_created(block));
    if (block < 0) {
        test_exit();
        return 0;
    }

    sizes_and_ranges(&vertex, base);
    sizes_and_ranges(&fragment, base);
    block_types(&vertex);
    block_types(&fragment);
    wrong_unmaps(&vertex, &fragment, base);
    wrong_unmaps(&fragment, &vertex, base);
    mixed(base);
    offsets(&vertex, base);
    offsets(&fragment, base);

    // NULL pointers go last, so an implementation that crashes on them loses nothing else
    unsigned int offset = 0;
    PRINT("vertex USSE: a NULL base", sceGxmMapVertexUsseMemory(NULL, 4 * KIB, &offset));
    PRINT("fragment USSE: a NULL base", sceGxmMapFragmentUsseMemory(NULL, 4 * KIB, &offset));
    PRINT("vertex USSE: unmap a NULL base", sceGxmUnmapVertexUsseMemory(NULL));
    PRINT("fragment USSE: unmap a NULL base", sceGxmUnmapFragmentUsseMemory(NULL));
    sceKernelFreeMemBlock(block);
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
