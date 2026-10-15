/* dxgi.dll proxy: every export of the system dxgi.dll, forwarded lazily.
   CreateDXGIFactory/1/2 are implemented in hooks.c so the factory can be hooked. */
#include <windows.h>

static const char *const export_names[] = {
    "ApplyCompatResolutionQuirking", "CompatString", "CompatValue", "DXGIDumpJournal",
    "PIXBeginCapture", "PIXEndCapture", "PIXGetCaptureState", "SetAppCompatStringPointer",
    "UpdateHMDEmulationStatus", "DXGID3D10CreateDevice", "DXGID3D10CreateLayeredDevice",
    "DXGID3D10GetLayeredDeviceSize", "DXGID3D10RegisterLayers",
    "DXGIDeclareAdapterRemovalSupport", "DXGIDisableVBlankVirtualization",
    "DXGIGetDebugInterface1", "DXGIReportAdapterConfiguration",
};
#define EXPORT_COUNT (sizeof(export_names) / sizeof(export_names[0]))
static void *real_exports[EXPORT_COUNT];
static HMODULE system_dxgi;

HMODULE proxy_system_dxgi(void) {
    if (!system_dxgi) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH - 16);
        lstrcpyW(path + n, L"\\dxgi.dll");
        system_dxgi = LoadLibraryExW(path, NULL, 0);
    }
    return system_dxgi;
}

void *proxy_resolve(unsigned index) {
    if (index >= EXPORT_COUNT) {
        return NULL;
    }
    if (!real_exports[index] && proxy_system_dxgi()) {
        real_exports[index] = (void *)GetProcAddress(system_dxgi, export_names[index]);
    }
    return real_exports[index];
}

/* Saves the argument registers, resolves export eax, restores them and tail-jumps. */
__asm__(".text\n"
        ".p2align 4\n"
        "proxy_common:\n"
        "  push %rcx\n  push %rdx\n  push %r8\n  push %r9\n"
        "  sub $0x68, %rsp\n"
        "  movdqu %xmm0, 0x20(%rsp)\n  movdqu %xmm1, 0x30(%rsp)\n"
        "  movdqu %xmm2, 0x40(%rsp)\n  movdqu %xmm3, 0x50(%rsp)\n"
        "  mov %eax, %ecx\n"
        "  call proxy_resolve\n"
        "  movdqu 0x20(%rsp), %xmm0\n  movdqu 0x30(%rsp), %xmm1\n"
        "  movdqu 0x40(%rsp), %xmm2\n  movdqu 0x50(%rsp), %xmm3\n"
        "  add $0x68, %rsp\n"
        "  pop %r9\n  pop %r8\n  pop %rdx\n  pop %rcx\n"
        "  test %rax, %rax\n  jz 1f\n"
        "  jmp *%rax\n"
        "1:\n  mov $0x80004005, %eax\n  ret\n");

#define STUB(name, index)                                                                    \
    __declspec(dllexport) __attribute__((naked)) void name(void) {                           \
        __asm__ volatile("mov $" #index ", %eax\n  jmp proxy_common\n");                     \
    }

STUB(ApplyCompatResolutionQuirking, 0)
STUB(CompatString, 1)
STUB(CompatValue, 2)
STUB(DXGIDumpJournal, 3)
STUB(PIXBeginCapture, 4)
STUB(PIXEndCapture, 5)
STUB(PIXGetCaptureState, 6)
STUB(SetAppCompatStringPointer, 7)
STUB(UpdateHMDEmulationStatus, 8)
STUB(DXGID3D10CreateDevice, 9)
STUB(DXGID3D10CreateLayeredDevice, 10)
STUB(DXGID3D10GetLayeredDeviceSize, 11)
STUB(DXGID3D10RegisterLayers, 12)
STUB(DXGIDeclareAdapterRemovalSupport, 13)
STUB(DXGIDisableVBlankVirtualization, 14)
STUB(DXGIGetDebugInterface1, 15)
STUB(DXGIReportAdapterConfiguration, 16)
