// How SceFios2Kernel resolves a path through a process's overlays: each overlay type for reads and
// writes, where a dst prefix matches, several overlays on one path, order ranges, and the sizes in
// the argument block
#include "fios.h"

#define R FIOS_DIR "/r"

static char out[0x500];

static void resolve_range(const char *label, int write, const char *in, int min_order, int max_order) {
    sceClibMemset(out, 0, sizeof(out));
    const int ret = fios_resolve_range(write, in, out, 0x100, min_order, max_order);
    PRINTF(ret, "%s %s %s -> [%s]", label, write ? "write" : "read", fios_short(in), ret < 0 ? "" : fios_short(out));
}

static void resolve(const char *label, int write, const char *in) {
    resolve_range(label, write, in, 0, 0x7F);
}

static SceFiosOverlayID add(int type, int order, const char *dst, const char *src) {
    SceFiosOverlay overlay = fios_overlay(type, order, dst, src);
    SceFiosOverlayID id = -1;
    const int ret = fios_add(&overlay, &id);
    if (ret < 0)
        PRINT("add failed", ret);
    return id;
}

// Every case for one overlay type, from R/dst/ to R/src/
static void type_cases(const char *label, int type) {
    const SceFiosOverlayID id = add(type, 0, R "/dst/", R "/src/");
    for (int write = 0; write < 2; write++) {
        resolve(label, write, R "/dst/both");
        resolve(label, write, R "/dst/srconly");
        resolve(label, write, R "/dst/dstonly");
        resolve(label, write, R "/dst/missing");
        resolve(label, write, R "/dst/newer");
        resolve(label, write, R "/dst/older");
        resolve(label, write, R "/dst/subdir");
        resolve(label, write, R "/dst/");
        resolve(label, write, R "/dst");
    }
    fios_remove(id);
}

// Resolves R/dst/both with the argument block filled as given, into a buffer filled with '#'
static void sizes(const char *label, int write, SceSize max_path, SceSize in_size, SceSize out_size, const char *in) {
    sceClibMemset(out, '#', sizeof(out));
    FiosRangeArgs args = {
        .out_path = out,
        .max_path = max_path,
        .min_order = 0,
        .max_order = 0x7F,
        .in_path_size = in_size,
        .out_path_size = out_size,
    };
    const int ret = _sceFiosKernelOverlayResolveWithRangeSync(fios_pid, write, in, (SceFiosResolveWithRangeSyncSyscallArgs *)&args);
    int written = 0;
    while (written < (int)sizeof(out) && out[written] != '#')
        written++;
    int len = 0;
    while (len < written && out[len] != '\0')
        len++;
    PRINTF(ret, "%s", label);
    PRINTF(written, "%s: bytes written", label);
    PRINTF(len, "%s: length before a terminator", label);
}

int main(void) {
    fios_init();
    sceIoMkdir(FIOS_DIR, 0777);
    sceIoMkdir(R, 0777);
    sceIoMkdir(R "/dst", 0777);
    sceIoMkdir(R "/src", 0777);
    sceIoMkdir(R "/src2", 0777);
    sceIoMkdir(R "/src/subdir", 0777);
    fios_write_file(R "/dst/both");
    fios_write_file(R "/dst/dstonly");
    fios_write_file(R "/dst/newer");
    fios_write_file(R "/dst/older");
    fios_write_file(R "/src/both");
    fios_write_file(R "/src/srconly");
    fios_write_file(R "/src/newer");
    fios_write_file(R "/src/older");
    fios_write_file(R "/src2/both");
    fios_write_file(R "/src2/src2only");
    fios_set_mtime(R "/dst/both", 3);
    fios_set_mtime(R "/src/both", 3);
    fios_set_mtime(R "/dst/newer", 1);
    fios_set_mtime(R "/src/newer", 2);
    fios_set_mtime(R "/dst/older", 2);
    fios_set_mtime(R "/src/older", 1);

    // Each type
    resolve("no overlay", 0, R "/dst/both");
    type_cases("opaque", SCE_FIOS_OVERLAY_TYPE_OPAQUE);
    type_cases("translucent", SCE_FIOS_OVERLAY_TYPE_TRANSLUCENT);
    type_cases("newer", SCE_FIOS_OVERLAY_TYPE_NEWER);
    type_cases("writable", SCE_FIOS_OVERLAY_TYPE_WRITABLE);

    // Where dst matches
    SceFiosOverlayID id = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, R "/dst/", R "/src/");
    resolve("prefix", 0, R "/dstx/both");
    resolve("prefix", 0, R "/dst//both");
    resolve("prefix", 0, R "/dst/./both");
    resolve("prefix", 0, R "/DST/both");
    resolve("prefix", 0, "UX0:data/vita-autotests/fios/r/dst/both");
    resolve("prefix", 0, R "/src/both");
    resolve("prefix", 0, "ux0:/data/vita-autotests/fios/r/dst/both");
    fios_remove(id);
    id = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, R "/dst", R "/src");
    resolve("no slash", 0, R "/dst/both");
    resolve("no slash", 0, R "/dstx/both");
    resolve("no slash", 0, R "/dst");
    fios_remove(id);
    id = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, "app0:archive", "ux0:data");
    resolve("app0:archive", 0, "app0:archive/a");
    resolve("app0:archive", 0, "app0:/archive/a");
    fios_remove(id);
    id = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, "app0:/archive/", "ux0:data/");
    resolve("app0:/archive/", 0, "app0:archive/a");
    resolve("app0:/archive/", 0, "app0:/archive/a");
    fios_remove(id);
    id = add(SCE_FIOS_OVERLAY_TYPE_TRANSLUCENT, 0, "app0:/archive/", "app0:/archive/");
    resolve("same dst and src", 0, "app0:/archive/misc/global.not");
    resolve("same dst and src", 0, "app0:/eboot.bin");
    fios_remove(id);

    // Several overlays on one path
    SceFiosOverlayID a = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, R "/c1/", R "/c2/");
    SceFiosOverlayID b = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 1, R "/c2/", R "/c3/");
    resolve("chain 0 then 1", 0, R "/c1/f");
    resolve("chain 0 then 1", 0, R "/c2/f");
    fios_remove(a);
    fios_remove(b);
    a = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 1, R "/c1/", R "/c2/");
    b = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, R "/c2/", R "/c3/");
    resolve("chain 1 then 0", 0, R "/c1/f");
    fios_remove(a);
    fios_remove(b);
    a = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, R "/s/", R "/sa/");
    b = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 1, R "/s/", R "/sb/");
    resolve("same dst orders 0 and 1", 0, R "/s/f");
    fios_remove(a);
    fios_remove(b);
    a = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 1, R "/s/", R "/sa/");
    b = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, R "/s/", R "/sb/");
    resolve("same dst orders 1 and 0", 0, R "/s/f");
    fios_remove(a);
    fios_remove(b);
    a = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 2, R "/s/", R "/sa/");
    b = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 2, R "/s/", R "/sb/");
    resolve("same dst and order", 0, R "/s/f");
    fios_remove(a);
    fios_remove(b);
    a = add(SCE_FIOS_OVERLAY_TYPE_TRANSLUCENT, 0, R "/dst/", R "/src/");
    b = add(SCE_FIOS_OVERLAY_TYPE_TRANSLUCENT, 1, R "/dst/", R "/src2/");
    resolve("two translucent", 0, R "/dst/both");
    resolve("two translucent", 0, R "/dst/srconly");
    resolve("two translucent", 0, R "/dst/src2only");
    resolve("two translucent", 0, R "/dst/dstonly");
    fios_remove(a);
    fios_remove(b);
    a = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, R "/a/", R "/dst/");
    b = add(SCE_FIOS_OVERLAY_TYPE_TRANSLUCENT, 1, R "/dst/", R "/src/");
    resolve("opaque into translucent", 0, R "/a/srconly");
    resolve("opaque into translucent", 0, R "/a/dstonly");
    fios_remove(a);
    fios_remove(b);

    // Order ranges
    id = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 5, R "/g/", R "/h/");
    resolve_range("orders 0-4", 0, R "/g/f", 0, 4);
    resolve_range("orders 5-5", 0, R "/g/f", 5, 5);
    resolve_range("orders 6-0x7F", 0, R "/g/f", 6, 0x7F);
    resolve_range("orders 0-0xFF", 0, R "/g/f", 0, 0xFF);
    resolve_range("orders 0x7F-0", 0, R "/g/f", 0x7F, 0);
    PRINT("thread overlays disabled", _sceFiosKernelOverlayThreadIsDisabled());
    PRINT("disable them for the thread", _sceFiosKernelOverlayThreadSetDisabled(1));
    PRINT("thread overlays disabled", _sceFiosKernelOverlayThreadIsDisabled());
    resolve_range("disabled", 0, R "/g/f", 0, 0x7F);
    PRINT("enable them again", _sceFiosKernelOverlayThreadSetDisabled(0));
    resolve_range("enabled", 0, R "/g/f", 0, 0x7F);

    // _sceFiosKernelOverlayResolveSync, with every order
    FiosResolveArgs args = { .out_path = out, .max_path = 0x100, .out_path_size = 0x100 };
    args.in_path_size = sceClibStrnlen(R "/g/f", 0x100) + 1;
    sceClibMemset(out, 0, sizeof(out));
    int ret = _sceFiosKernelOverlayResolveSync(fios_pid, 0, R "/g/f", (SceFiosResolveSyncSyscallArgs *)&args);
    PRINTF(ret, "resolve sync D/r/g/f -> [%s]", ret < 0 ? "" : fios_short(out));
    fios_remove(id);

    // The sizes in the argument block, resolving R/dst/both to R/src/both
    id = add(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, R "/dst/", R "/src/");
    const char *in = R "/dst/both";
    const SceSize in_size = sceClibStrnlen(in, 0x100) + 1;
    const SceSize len = sceClibStrnlen(R "/src/both", 0x100);
    sizes("sizes as libfios2 gives them", 0, 0x100, in_size, 0x100, in);
    sizes("max path one past the result", 0, len + 1, in_size, len + 1, in);
    sizes("max path the result's length", 0, len, in_size, len + 1, in);
    sizes("max path one short", 0, len - 1, in_size, len + 1, in);
    sizes("max path shorter than the input", 0, in_size - 2, in_size, 0x100, in);
    sizes("out size the result's length", 0, 0x100, in_size, len, in);
    sizes("out size 4", 0, 0x100, in_size, 4, in);
    sizes("out size 0", 0, 0x100, in_size, 0, in);
    sizes("max path 0", 0, 0, in_size, 0x100, in);
    sizes("max path 0x400", 0, 0x400, in_size, 0x400, in);
    sizes("max path 0x401", 0, 0x401, in_size, 0x100, in);
    sizes("out size 0x401", 0, 0x100, in_size, 0x401, in);
    sizes("in size 0", 0, 0x100, 0, 0x100, in);
    sizes("in size without the terminator", 0, 0x100, in_size - 1, 0x100, in);
    sizes("in size 4", 0, 0x100, 4, 0x100, in);
    sizes("in size 0x100", 0, 0x100, 0x100, 0x100, in);
    sizes("write flag 2", 2, 0x100, in_size, 0x100, in);
    sizes("write flag -1", -1, 0x100, in_size, 0x100, in);
    fios_remove(id);

    test_exit();
    return 0;
}
