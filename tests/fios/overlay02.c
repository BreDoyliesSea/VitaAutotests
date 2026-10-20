// The SceFios2Kernel "02" syscalls newer libfios2 versions call, next to the older ones: whether they
// share one overlay table, and how their resolve block handles sizes
#include "fios.h"

#define R FIOS_DIR "/r02"

static char out[0x500];

static int resolve02(int write, const char *in, SceSize max_path, SceSize out_size, int min_order, int max_order) {
    sceClibMemset(out, '#', sizeof(out));
    sceFiosKernelOverlayResolveWithRangeSync02_opt opt;
    sceClibMemset(&opt, 0, sizeof(opt));
    opt.out_path = out;
    opt.max_path = max_path;
    opt.min_order = min_order;
    opt.max_order = max_order;
    opt.out_path_buffer_size = out_size;
    return sceFiosKernelOverlayResolveWithRangeSync02(fios_pid, write, in, &opt);
}

static void show02(const char *label, int ret) {
    int written = 0;
    while (written < (int)sizeof(out) && out[written] != '#')
        written++;
    int len = 0;
    while (len < written && out[len] != '\0')
        len++;
    PRINTF(ret, "%s -> [%s]", label, ret < 0 || len == written ? "" : fios_short(out));
    PRINTF(written, "%s: bytes written", label);
}

int main(void) {
    fios_init();
    sceIoMkdir(FIOS_DIR, 0777);
    sceIoMkdir(R, 0777);
    sceIoMkdir(R "/dst", 0777);
    sceIoMkdir(R "/src", 0777);
    fios_write_file(R "/dst/dstonly");
    fios_write_file(R "/src/srconly");

    // One table or two
    SceFiosOverlay overlay = fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, R "/a/", R "/b/");
    SceFiosOverlayID old_id = 0x7777, new_id = 0x7777;
    PRINT("add with the old call", fios_add(&overlay, &old_id));
    overlay = fios_overlay(SCE_FIOS_OVERLAY_TYPE_TRANSLUCENT, 0, R "/dst/", R "/src/");
    PRINT("add with the 02 call", sceFiosKernelOverlayAddForProcess02(fios_pid, &overlay, &new_id));
    PRINT("02 id minus old id", new_id - old_id);
    SceFiosOverlay info;
    sceClibMemset(&info, 0, sizeof(info));
    int ret = sceFiosKernelOverlayGetInfoForProcess02(fios_pid, old_id, &info);
    PRINTF(ret, "02 info of the old one: dst [%s]", ret < 0 ? "" : fios_short(info.dst));
    sceClibMemset(&info, 0, sizeof(info));
    ret = _sceFiosKernelOverlayGetInfoForProcess(fios_pid, new_id, (SceFiosKernelOverlay *)&info);
    PRINTF(ret, "old info of the 02 one: dst [%s]", ret < 0 ? "" : fios_short(info.dst));
    SceSize actual = 0x7777;
    SceFiosOverlayID ids[4];
    const sceFiosKernelOverlayGetList02_opt list = { .out_ids = ids, .max_ids = 4, .actual_ids = &actual, .out_ids_buffer_size = sizeof(ids) };
    PRINT("02 list", sceFiosKernelOverlayGetList02(fios_pid, 0, 0x7F, &list));
    PRINT("overlays listed", actual);

    // Resolving with each call
    show02("02 read D/r02/a/f", resolve02(0, R "/a/f", 0x100, 0x100, 0, 0x7F));
    show02("02 read D/r02/dst/srconly", resolve02(0, R "/dst/srconly", 0x100, 0x100, 0, 0x7F));
    show02("02 read D/r02/dst/dstonly", resolve02(0, R "/dst/dstonly", 0x100, 0x100, 0, 0x7F));
    show02("02 write D/r02/dst/dstonly", resolve02(1, R "/dst/dstonly", 0x100, 0x100, 0, 0x7F));
    sceClibMemset(out, 0, sizeof(out));
    ret = fios_resolve_range(0, R "/dst/srconly", out, 0x100, 0, 0x7F);
    PRINTF(ret, "old read D/r02/dst/srconly -> [%s]", ret < 0 ? "" : fios_short(out));
    sceClibMemset(out, '#', sizeof(out));
    sceFiosKernelOverlayResolveSync02_opt sync;
    sceClibMemset(&sync, 0, sizeof(sync));
    sync.out_path = out;
    sync.max_path = 0x100;
    sync.out_path_buffer_size = 0x100;
    show02("02 sync read D/r02/a/f", sceFiosKernelOverlayResolveSync02(fios_pid, 0, R "/a/f", &sync));

    // Sizes
    const SceSize len = sceClibStrnlen(R "/b/f", 0x100);
    show02("02 max path one past the result", resolve02(0, R "/a/f", len + 1, len + 1, 0, 0x7F));
    show02("02 max path the result's length", resolve02(0, R "/a/f", len, len + 1, 0, 0x7F));
    show02("02 out size the result's length", resolve02(0, R "/a/f", 0x100, len, 0, 0x7F));
    show02("02 out size 0", resolve02(0, R "/a/f", 0x100, 0, 0, 0x7F));
    show02("02 max path 0", resolve02(0, R "/a/f", 0, 0x100, 0, 0x7F));
    show02("02 max path 0x401", resolve02(0, R "/a/f", 0x401, 0x100, 0, 0x7F));
    show02("02 out size 0x401", resolve02(0, R "/a/f", 0x100, 0x401, 0, 0x7F));
    show02("02 orders 0x7F-0", resolve02(0, R "/a/f", 0x100, 0x100, 0x7F, 0));

    // Removing across calls
    PRINT("02 remove of the old one", sceFiosKernelOverlayRemoveForProcess02(fios_pid, old_id));
    PRINT("old remove of the 02 one", fios_remove(new_id));
    PRINT("old remove of the old one again", fios_remove(old_id));

    test_exit();
    return 0;
}
