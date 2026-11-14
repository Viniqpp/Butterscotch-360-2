// Butterscotch XDK smoke test: the smallest XEX that exercises the same
// toolchain pieces as the real port (xtl.h, compat_msvc.h force-include,
// D3D9, XInput, XAudio2 creation, CRT file I/O) without the runner.
//
// On the console: the screen cycles colours. A = freeze colour, Back = exit.
// It writes game:\smoke.txt with the compiler/ABI facts the port relies on.

#include <xtl.h>
#include <xaudio2.h>
#include <cstdio>
#include <cstring>

extern "C" ULONG __cdecl DbgPrint(const char* format, ...); // kernel export, not in xtl.h

#define SMOKE_STR2(x) #x
#define SMOKE_STR(x) SMOKE_STR2(x)
#pragma message("smoke: _MSC_VER=" SMOKE_STR(_MSC_VER))
#if defined(_WIN32)
#pragma message("smoke: _WIN32 defined (src/xbox360-xdk/windows.h shim is required)")
#endif
#if !defined(_XBOX)
#error "_XBOX is not defined: this is not the Xbox 360 XDK compiler/platform"
#endif

static bool hostIsBigEndian(void) {
    const unsigned int probe = 0x01020304u;
    return *(const unsigned char*)&probe == 0x01;
}

static void writeReport(bool d3dOk, bool audioOk) {
    FILE* f = fopen("game:\\smoke.txt", "w");
    if (!f) return;
    fprintf(f, "Butterscotch XDK smoke test\n");
    fprintf(f, "_MSC_VER=%d\n", (int)_MSC_VER);
#if defined(_WIN32)
    fprintf(f, "_WIN32=1\n");
#else
    fprintf(f, "_WIN32=0\n");
#endif
#if defined(_M_PPC)
    fprintf(f, "_M_PPC=1\n");
#else
    fprintf(f, "_M_PPC=0\n");
#endif
    fprintf(f, "big_endian=%d (port requires 1)\n", hostIsBigEndian() ? 1 : 0);
    fprintf(f, "sizeof(void*)=%u sizeof(long)=%u sizeof(double)=%u\n",
            (unsigned)sizeof(void*), (unsigned)sizeof(long), (unsigned)sizeof(double));
    // compat_msvc.h polyfills used by the core when compiled as C++.
    volatile double zero = 0.0;
    fprintf(f, "isnan(0/0)=%d isfinite(1)=%d log2(8)=%.1f exp2(3)=%.1f\n",
            isnan(zero / zero) ? 1 : 0, isfinite(1.0) ? 1 : 0, log2(8.0), exp2(3.0));
    fprintf(f, "d3d9=%s xaudio2=%s\n", d3dOk ? "ok" : "FAILED", audioOk ? "ok" : "FAILED");
    fclose(f);
}

VOID __cdecl main() {
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9* device = NULL;
    if (d3d) {
        D3DPRESENT_PARAMETERS pp;
        ZeroMemory(&pp, sizeof(pp));
        pp.BackBufferWidth = 1280;
        pp.BackBufferHeight = 720;
        pp.BackBufferFormat = D3DFMT_A8R8G8B8;
        pp.FrontBufferFormat = D3DFMT_LE_X8R8G8B8;
        pp.BackBufferCount = 1;
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device)))
            device = NULL;
    }

    IXAudio2* xaudio = NULL;
    IXAudio2MasteringVoice* master = NULL;
    bool audioOk = SUCCEEDED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) &&
                   SUCCEEDED(xaudio->CreateMasteringVoice(&master));

    writeReport(device != NULL, audioOk);
    DbgPrint("BS-SMOKE: d3d9=%d xaudio2=%d\n", device != NULL, audioOk);

    DWORD frame = 0;
    bool frozen = false;
    WORD previous = 0;
    while (device) {
        XINPUT_STATE pad;
        ZeroMemory(&pad, sizeof(pad));
        WORD buttons = XInputGetState(0, &pad) == ERROR_SUCCESS ? pad.Gamepad.wButtons : 0;
        if ((buttons & XINPUT_GAMEPAD_BACK) != 0) break;
        if ((buttons & XINPUT_GAMEPAD_A) && !(previous & XINPUT_GAMEPAD_A)) frozen = !frozen;
        previous = buttons;
        if (!frozen) frame++;

        BYTE r = (BYTE)(frame * 2), g = (BYTE)(frame * 3), b = (BYTE)(frame * 5);
        device->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(r, g, b), 1.0f, 0);
        device->Present(NULL, NULL, NULL, NULL);
    }

    if (master) master->DestroyVoice();
    if (xaudio) xaudio->Release();
    if (device) device->Release();
    if (d3d) d3d->Release();
}
