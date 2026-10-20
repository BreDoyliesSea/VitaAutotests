// The overlay table SceFios2Kernel keeps for a process: the IDs it hands out, what it stores for an
// overlay, which overlays it refuses, the order it lists them in, and removing them
#include "fios.h"

// Prints what the table holds for id
static void show_info(const char *label, SceFiosOverlayID id) {
    SceFiosOverlay info;
    sceClibMemset(&info, 0xCC, sizeof(info));
    const int ret = _sceFiosKernelOverlayGetInfoForProcess(fios_pid, id, (SceFiosKernelOverlay *)&info);
    PRINTF(ret, "%s: get info", label);
    if (ret < 0)
        return;
    PRINTF(info.type, "%s: type", label);
    PRINTF(info.order, "%s: order", label);
    PRINTF(info.dst_len, "%s: dst length", label);
    PRINTF(info.src_len, "%s: src length", label);
    PRINTF(info.unk2, "%s: field 0xA", label);
    PRINTF(info.pid == fios_pid, "%s: pid is the process", label);
    PRINTF(info.id == id, "%s: id is its id", label);
    PRINTF(0, "%s: dst [%s]", label, fios_short(info.dst));
    PRINTF(0, "%s: src [%s]", label, fios_short(info.src));
}

// Adds an overlay, prints what the table stores for it, and removes it again
static void add_show_remove(const char *label, SceFiosOverlay overlay) {
    SceFiosOverlayID id = 0x7777;
    const int ret = fios_add(&overlay, &id);
    PRINTF(ret, "%s: add", label);
    PRINTF(id != 0x7777, "%s: id written", label);
    if (ret < 0)
        return;
    show_info(label, id);
    fios_remove(id);
}

// Adds an overlay that should be refused, and removes it if it wasn't
static void add_refused(const char *label, SceFiosOverlay overlay) {
    SceFiosOverlayID id = 0x7777;
    const int ret = fios_add(&overlay, &id);
    PRINTF(ret, "%s: add", label);
    PRINTF(id != 0x7777, "%s: id written", label);
    if (ret >= 0)
        fios_remove(id);
}

static int list_ids(int min_order, int max_order, SceFiosOverlayID *ids, SceSize max_ids, SceSize *actual) {
    FiosListArgs args = {
        .out_ids = ids,
        .max_ids = max_ids,
        .actual_ids = actual,
        .out_ids_size = max_ids * sizeof(SceFiosOverlayID),
    };
    return _sceFiosKernelOverlayGetList(fios_pid, min_order, max_order, (SceFiosGetListSyscallArgs *)&args);
}

int main(void) {
    fios_init();
    SceFiosOverlay overlay;

    // IDs
    SceFiosOverlayID first = 0x7777, second = 0x7777, third = 0x7777;
    overlay = fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, FIOS_DIR "/a/", FIOS_DIR "/b/");
    PRINT("add an overlay", fios_add(&overlay, &first));
    PRINT("its id is positive", first > 0);
    overlay = fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, FIOS_DIR "/a/", FIOS_DIR "/b/");
    PRINT("add the same overlay again", fios_add(&overlay, &second));
    PRINT("second id minus first", second - first);
    show_info("first overlay", first);

    // Removing
    PRINT("remove the first", fios_remove(first));
    PRINT("remove the first again", fios_remove(first));
    SceFiosOverlay info;
    PRINT("get info of the removed one", _sceFiosKernelOverlayGetInfoForProcess(fios_pid, first, (SceFiosKernelOverlay *)&info));
    overlay = fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, FIOS_DIR "/a/", FIOS_DIR "/b/");
    PRINT("add after removing", fios_add(&overlay, &third));
    PRINT("third id minus first", third - first);
    PRINT("remove the second", fios_remove(second));
    PRINT("remove the third", fios_remove(third));
    PRINT("remove id 0", fios_remove(0));
    PRINT("remove id -1", fios_remove(-1));
    PRINT("remove an id never handed out", fios_remove(third + 100));

    // What gets stored
    add_show_remove("translucent", fios_overlay(SCE_FIOS_OVERLAY_TYPE_TRANSLUCENT, 3, FIOS_DIR "/a/", FIOS_DIR "/b/"));
    add_show_remove("newer", fios_overlay(SCE_FIOS_OVERLAY_TYPE_NEWER, 0x10, FIOS_DIR "/a/", FIOS_DIR "/b/"));
    add_show_remove("writable", fios_overlay(SCE_FIOS_OVERLAY_TYPE_WRITABLE, 0x7F, FIOS_DIR "/a/", FIOS_DIR "/b/"));
    add_show_remove("dst the same as src", fios_overlay(SCE_FIOS_OVERLAY_TYPE_TRANSLUCENT, 0, "app0:/archive/", "app0:/archive/"));
    add_show_remove("no slash after the colon", fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, "app0:archive", "ux0:data"));
    add_show_remove("no trailing slash", fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, FIOS_DIR "/a", FIOS_DIR "/b"));
    add_show_remove("two slashes", fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, FIOS_DIR "//a//", FIOS_DIR "/b//"));
    add_show_remove("upper case device", fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, "UX0:data/a/", "APP0:/b/"));
    add_show_remove("device only", fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, "app0:", "ux0:"));
    add_show_remove("no device", fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, "/archive/", "/other/"));
    overlay = fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, FIOS_DIR "/a/", FIOS_DIR "/b/");
    overlay.dst_len = 3;
    overlay.src_len = 0xFFFF;
    overlay.unk2 = 0x1234;
    overlay.pid = 0x5678;
    overlay.id = 0x9ABC;
    add_show_remove("wrong lengths, pid and id", overlay);

    // Refused overlays
    add_refused("type 4", fios_overlay(4, 0, FIOS_DIR "/a/", FIOS_DIR "/b/"));
    add_refused("type 0xFF", fios_overlay(0xFF, 0, FIOS_DIR "/a/", FIOS_DIR "/b/"));
    add_refused("order 0x80", fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0x80, FIOS_DIR "/a/", FIOS_DIR "/b/"));
    add_refused("order 0xFF", fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0xFF, FIOS_DIR "/a/", FIOS_DIR "/b/"));
    add_refused("empty dst", fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, "", FIOS_DIR "/b/"));
    add_refused("empty src", fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, FIOS_DIR "/a/", ""));
    overlay = fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, "", FIOS_DIR "/b/");
    sceClibMemset(overlay.dst, 'a', sizeof(overlay.dst) - 1);
    add_refused("dst of 291 characters", overlay);
    sceClibMemset(overlay.dst, 'a', sizeof(overlay.dst));
    add_refused("dst without a terminator", overlay);

    SceFiosOverlayID id = 0x7777;
    overlay = fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, FIOS_DIR "/a/", FIOS_DIR "/b/");
    int ret = _sceFiosKernelOverlayAddForProcess(0, (const SceFiosKernelOverlay *)&overlay, &id);
    PRINT("add for process 0", ret);
    if (ret >= 0)
        PRINT("remove it for process 0", _sceFiosKernelOverlayRemoveForProcess(0, id));
    ret = _sceFiosKernelOverlayAddForProcess(sceKernelGetThreadId(), (const SceFiosKernelOverlay *)&overlay, &id);
    PRINT("add for a thread's UID", ret);
    if (ret >= 0)
        fios_remove(id);
    ret = _sceFiosKernelOverlayAdd((const SceFiosKernelOverlay *)&overlay, &id);
    PRINT("add without a process", ret);
    if (ret >= 0) {
        show_info("added without a process", id);
        fios_remove(id);
    }

    // The order of the list
    SceFiosOverlayID ids[8], listed[8];
    const int orders[] = { 5, 1, 3, 1, 0x7F };
    for (int i = 0; i < 5; i++) {
        overlay = fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, orders[i], FIOS_DIR "/a/", FIOS_DIR "/b/");
        fios_add(&overlay, &ids[i]);
    }
    SceSize actual = 0x7777;
    PRINT("list every order", list_ids(0, 0x7F, listed, 8, &actual));
    PRINT("overlays listed", actual);
    for (SceSize i = 0; i < actual && i < 8; i++) {
        int added = -1;
        for (int j = 0; j < 5; j++)
            if (listed[i] == ids[j])
                added = j;
        PRINTF(added, "listed %d is the one added at", (int)i);
    }
    actual = 0x7777;
    PRINT("list orders 1 to 3", list_ids(1, 3, listed, 8, &actual));
    PRINT("overlays listed", actual);
    actual = 0x7777;
    PRINT("list with room for 2", list_ids(0, 0x7F, listed, 2, &actual));
    PRINT("overlays counted", actual);
    actual = 0x7777;
    PRINT("list with min above max", list_ids(5, 1, listed, 8, &actual));
    PRINT("overlays counted", actual);
    for (int i = 0; i < 5; i++)
        fios_remove(ids[i]);

    // How many fit
    static SceFiosOverlayID many[300];
    int count = 0;
    for (; count < 300; count++) {
        overlay = fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, FIOS_DIR "/a/", FIOS_DIR "/b/");
        ret = fios_add(&overlay, &many[count]);
        if (ret < 0)
            break;
    }
    PRINT("overlays added before an error", count);
    PRINT("the error", count < 300 ? ret : 0);
    for (int i = 0; i < count; i++)
        fios_remove(many[i]);

    // A NULL id. A NULL overlay, or a NULL info buffer, crashes a Vita, so they are not tried
    overlay = fios_overlay(SCE_FIOS_OVERLAY_TYPE_OPAQUE, 0, FIOS_DIR "/a/", FIOS_DIR "/b/");
    ret = fios_add(&overlay, NULL);
    PRINT("add with a NULL id", ret);

    test_exit();
    return 0;
}
