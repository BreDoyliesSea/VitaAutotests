// sceGxmMapMemory on a whole block of each type that gxm_map_memory leaves out, with READ, WRITE,
// and both: USER_CDRAM_L1WBWA_RW, the read only USER_CDRAM_R, USER_MAIN_DEVICE_RW,
// USER_MAIN_GAME_RW, the physically contiguous types and the common dialog types. A type that
// fails to allocate prints its error. Then a 1 MiB block for code from
// sceKernelAllocMemBlockForVM: mapped with the VM domain closed and with it open, opening and
// closing the domain while it's mapped, and last, freeing it with sceKernelFreeMemBlockForVM while
// it's mapped, what its address finds, the unmap and freeing it again. That goes last, since an
// implementation whose free ends a mapped block may crash on the unmap. Every other map that works
// is unmapped again.
//
// On a PS Vita TV on firmware 3.65 every type maps with READ, WRITE and both, except the read only
// types, USER_CDRAM_R and the two read only common dialog types, which take only READ, and
// USER_MAIN_DEVICE_RW, which never maps (0x805B0017, DRIVER). A block for code never maps either,
// with the VM domain closed or open, so sceKernelFreeMemBlockForVM frees it, after which its
// address is in no block (0x80022007) and a second free gives 0x80020005 (INVALID_ARGUMENT).
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define RW (SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)

// vitasdk has a stub for it but no prototype
int sceKernelFreeMemBlockForVM(SceUID uid);

static const struct {
    SceUInt32 type;
    SceSize unit;
} types[] = {
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_L1WBWA_RW, 256 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_R, 256 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_DEVICE_RW, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_GAME_RW, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, MIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, MIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_RW, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_NC_RW, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_R, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_RW, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_NC_R, 4 * KIB },
    { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_NC_RW, 4 * KIB },
};

static const struct {
    SceUInt32 attribs;
    const char *name;
} attribs[] = {
    { SCE_GXM_MEMORY_ATTRIB_READ, "READ" },
    { SCE_GXM_MEMORY_ATTRIB_WRITE, "WRITE" },
    { RW, "READ and WRITE" },
};

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

// Maps the range with READ, WRITE, and both, one after another, unmapping each map that works
static void map_each_attrib(const char *what, void *base, SceSize size) {
    for (unsigned int i = 0; i < sizeof(attribs) / sizeof(attribs[0]); i++) {
        const int mapped = sceGxmMapMemory(base, size, attribs[i].attribs);
        PRINTF(mapped, "%s with %s", what, attribs[i].name);
        if (mapped == 0)
            PRINTF(sceGxmUnmapMemory(base), "%s with %s: unmap it", what, attribs[i].name);
    }
}

static void map_type(SceUInt32 type, SceSize size) {
    const SceUID block = sceKernelAllocMemBlock("gxm type", type, size, NULL);
    PRINTF(test_created(block), "type 0x%08X: allocate it", (unsigned)type);
    if (block < 0)
        return;
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    char what[32];
    sceClibSnprintf(what, sizeof(what), "type 0x%08X", (unsigned)type);
    map_each_attrib(what, base, size);
    sceKernelFreeMemBlock(block);
}

static void for_vm(void) {
    const SceUID block = sceKernelAllocMemBlockForVM("gxm vm", MIB);
    PRINT("allocate a block for code", test_created(block));
    if (block < 0)
        return;
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    map_each_attrib("a block for code, the domain closed", base, MIB);

    PRINT("open the domain", sceKernelOpenVMDomain());
    map_each_attrib("a block for code, the domain open", base, MIB);
    PRINT("close the domain", sceKernelCloseVMDomain());

    const int mapped = sceGxmMapMemory(base, MIB, SCE_GXM_MEMORY_ATTRIB_READ);
    PRINT("map it with READ to keep it mapped", mapped);
    PRINT("open the domain while it's mapped", sceKernelOpenVMDomain());
    PRINT("close the domain while it's mapped", sceKernelCloseVMDomain());
    if (mapped == 0)
        PRINT("unmap it after that", sceGxmUnmapMemory(base));

    // Last, since the free may end the block while it's mapped
    const int again = sceGxmMapMemory(base, MIB, SCE_GXM_MEMORY_ATTRIB_READ);
    PRINT("map it with READ before freeing it", again);
    PRINT("free it with the call for code while it's mapped", sceKernelFreeMemBlockForVM(block));
    const SceUID found = sceKernelFindMemBlockByAddr(base, 0);
    PRINT("find it by its address after the free", found == block ? 0 : found);
    if (again == 0)
        PRINT("unmap it after the free", sceGxmUnmapMemory(base));
    PRINT("free it with the call for code after the unmap", sceKernelFreeMemBlockForVM(block));
}

int main(void) {
    PRINT("initialize libgxm", initialize());
    for (unsigned int i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        map_type(types[i].type, types[i].unit);
    for_vm();
    PRINT("terminate libgxm", sceGxmTerminate());
    test_exit();
    return 0;
}
