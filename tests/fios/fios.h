// Helpers for the FIOS overlay tests. They call the SceFios2Kernel syscalls the way the libfios2
// that older games bundle does, filling the argument blocks the same way.
#pragma once

#include <psp2/fios2kernel.h>
#include <psp2/fios2kernel02.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define FIOS_DIR "ux0:data/vita-autotests/fios"

// The block _sceFiosKernelOverlayResolveWithRangeSync takes, as that libfios2 fills it
typedef struct FiosRangeArgs {
    char *out_path;
    SceSize max_path;
    SceUInt8 min_order;
    SceUInt8 max_order;
    SceUInt8 reserved[2];
    SceSize in_path_size; // strlen(in_path) + 1
    SceSize out_path_size; // the size of out_path, the same as max_path
    SceUInt32 unused;
} FiosRangeArgs;

// The block _sceFiosKernelOverlayResolveSync takes, as that libfios2 fills it
typedef struct FiosResolveArgs {
    char *out_path;
    SceSize max_path;
    SceSize in_path_size;
    SceSize out_path_size;
    SceUInt32 unused[2];
} FiosResolveArgs;

// The block _sceFiosKernelOverlayGetList takes, as that libfios2 fills it
typedef struct FiosListArgs {
    SceFiosOverlayID *out_ids;
    SceSize max_ids;
    SceSize *actual_ids;
    SceSize out_ids_size; // max_ids * 4, or 0x200 past 128 IDs
    SceUInt32 unused[2];
} FiosListArgs;

static SceUID fios_pid;

static inline void fios_init(void) {
    fios_pid = sceKernelGetProcessId();
}

static inline SceFiosOverlay fios_overlay(int type, int order, const char *dst, const char *src) {
    SceFiosOverlay overlay;
    sceClibMemset(&overlay, 0, sizeof(overlay));
    overlay.type = type;
    overlay.order = order;
    sceClibStrncpy(overlay.dst, dst, sizeof(overlay.dst) - 1);
    sceClibStrncpy(overlay.src, src, sizeof(overlay.src) - 1);
    overlay.dst_len = sceClibStrnlen(overlay.dst, sizeof(overlay.dst));
    overlay.src_len = sceClibStrnlen(overlay.src, sizeof(overlay.src));
    overlay.pid = fios_pid;
    return overlay;
}

static inline int fios_add(SceFiosOverlay *overlay, SceFiosOverlayID *id) {
    return _sceFiosKernelOverlayAddForProcess(fios_pid, (const SceFiosKernelOverlay *)overlay, id);
}

static inline int fios_remove(SceFiosOverlayID id) {
    return _sceFiosKernelOverlayRemoveForProcess(fios_pid, id);
}

static inline int fios_resolve_range(int write, const char *in, char *out, SceSize size, int min_order, int max_order) {
    FiosRangeArgs args = {
        .out_path = out,
        .max_path = size,
        .min_order = min_order,
        .max_order = max_order,
        .in_path_size = in ? sceClibStrnlen(in, 0x1000) + 1 : 0,
        .out_path_size = size,
    };
    return _sceFiosKernelOverlayResolveWithRangeSync(fios_pid, write, in, (SceFiosResolveWithRangeSyncSyscallArgs *)&args);
}

// Returns path with FIOS_DIR shortened to D, so the lines stay short. FIOS resolves ux0:data to ux0:/data.
static inline const char *fios_short(const char *path) {
    static char buf[2][96];
    static int which;
    char *out = buf[which ^= 1];
    static const char *const prefixes[] = { FIOS_DIR, "ux0:/data/vita-autotests/fios" };
    for (int i = 0; i < 2; i++) {
        const int len = sceClibStrnlen(prefixes[i], 64);
        if (sceClibStrncmp(path, prefixes[i], len) == 0) {
            sceClibSnprintf(out, sizeof(buf[0]), "D%s", path + len);
            return out;
        }
    }
    sceClibSnprintf(out, sizeof(buf[0]), "%s", path);
    return out;
}

static inline void fios_write_file(const char *path) {
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    sceIoWrite(fd, "fios", 4);
    sceIoClose(fd);
}

static inline void fios_set_mtime(const char *path, int day) {
    SceIoStat stat;
    sceClibMemset(&stat, 0, sizeof(stat));
    stat.st_mtime.year = 2020;
    stat.st_mtime.month = 1;
    stat.st_mtime.day = day;
    stat.st_mtime.hour = 12;
    sceIoChstat(path, &stat, SCE_CST_MT);
}
