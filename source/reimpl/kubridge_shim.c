// kubridge shim for Vita3K / environments without the kubridge.skprx kernel plugin.
// On real hardware, kubridge.skprx is loaded as a kernel plugin and provides
// these functions via import stubs. We only define them when VITA3K_BUILD is set,
// so they don't shadow the real kubridge on real hardware.

#ifdef VITA3K_BUILD

#include <psp2/kernel/sysmem.h>
#include <psp2/types.h>
#include <string.h>

#include "../../lib/kubridge/kubridge.h"

SceUID kuKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                             SceSize size, SceKernelAllocMemBlockKernelOpt *opt) {
    (void)opt;
    return sceKernelAllocMemBlock(name, type, size, NULL);
}

int kuKernelCpuUnrestrictedMemcpy(void *dst, const void *src, SceSize len) {
    memcpy(dst, src, len);
    return 0;
}

void kuKernelFlushCaches(const void *ptr, SceSize len) {
    (void)ptr;
    (void)len;
}

int kuKernelRegisterAbortHandler(KuKernelAbortHandler pHandler, KuKernelAbortHandler *pOldHandler) {
    (void)pHandler;
    if (pOldHandler) *pOldHandler = NULL;
    return 0;
}

void kuKernelReleaseAbortHandler(void) {
}

// sceGxmVshInitialize shim — Vita3K stubs this to a no-op but vitaGL needs it.
// Redirect to the standard sceGxmInitialize that Vita3K fully implements.
// NOTE: Only valid with vitaGL built with HAVE_VITA3K_SUPPORT=1 which uses
// SCE_GXM_INITIALIZE_FLAG_DEFAULT instead of EXTENDED_FORMAT.
#include <psp2/gxm.h>
int sceGxmVshInitialize(const SceGxmInitializeParams *params) {
    return sceGxmInitialize(params);
}

#endif // VITA3K_BUILD

// SceShaccCgExt stubs — only for Vita3K where the real library fails due to
// NID version mismatch. On real hardware, we link against libSceShaccCgExt.a
// (the real import stub).
#ifdef VITA3K_BUILD
int sceShaccCgExtEnableExtensions(void) { return 0; }
int sceShaccCgExtDisableExtensions(void) { return 0; }
#endif
