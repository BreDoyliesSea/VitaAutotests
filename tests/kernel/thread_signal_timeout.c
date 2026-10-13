// Waits for a thread signal that has none to take: they time out, and what they write back. A test
// of its own, since an implementation that ignores the timeout hangs on them.
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/signal.h>

#include "test.h"

int main(void) {
    const SceUID self = sceKernelGetThreadId();
    SceUInt32 timeout = 1000;
    PRINT("wait with no signal sent", sceKernelWaitSignal(0, 0, &timeout));
    PRINT("its time left", timeout);
    sceKernelSendSignal(self);
    sceKernelWaitSignal(0, 0, NULL);
    timeout = 1000;
    PRINT("wait again after taking the signal", sceKernelWaitSignal(0, 0, &timeout));
    PRINT("send a signal to UID 0", sceKernelSendSignal(0));
    timeout = 1000;
    PRINT("wait after a signal to UID 0", sceKernelWaitSignal(0, 0, &timeout));

    test_exit();
    return 0;
}
