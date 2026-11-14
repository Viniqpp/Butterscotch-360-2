// Butterscotch Xbox 360 — XDK Entry Point
// Uses official Xbox 360 SDK: D3D9, XAudio2, XInputGetState

#include <xtl.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// DbgPrint is a C-linkage kernel function — declare it explicitly since
// we compile .c files as C++ and removed extern "C" wrappers.
extern "C" ULONG __cdecl DbgPrint(const char* format, ...);

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "runner.h"
#include "runner_keyboard.h"
#include "runner_gamepad.h"
#include "vm.h"
#include "data_win.h"
#include "json_reader.h"
#include "utils.h"
#include "log.h"
#include "stb_ds.h"

#include "d3d9_renderer.h"
#include "xaudio2_audio.h"
#include "xdk_file_system.h"

// The core's log.c forwards every log line to the platform (cli/main.c has the desktop one).
void platformLog(const logType type, const char* format, va_list va) {
    char line[1024];
    vsnprintf(line, sizeof(line), format, va); // compat_msvc.h wrapper: always terminated
    OutputDebugStringA(type == LOG_TYPE_ERROR ? "BS error: " : type == LOG_TYPE_WARNING ? "BS warning: " : "BS: ");
    OutputDebugStringA(line);
}

// ===[ POSIX clock polyfill implementation ]===
double _xdk_monotonic_ms(void) {
    LARGE_INTEGER freq, counter;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter)) {
        return (double)GetTickCount();
    }
    return (double)counter.QuadPart / (double)freq.QuadPart * 1000.0;
}

// Screen dimensions (720p native)
#define SCREEN_WIDTH  1280
#define SCREEN_HEIGHT 720

// ===[ Controller Mapping ]===

typedef struct {
    WORD xpadButton;
    int32_t gmlKey;
} XpadMapping;

static XpadMapping* xpadMappings = NULL;
static int xpadMappingCount = 0;
static WORD prevButtons = 0;
static BYTE prevLeftTrigger = 0;
static BYTE prevRightTrigger = 0;

static void setupDefaultMappings(void) {
    static XpadMapping defaults[] = {
        { XINPUT_GAMEPAD_DPAD_UP,    38 },  // VK_UP
        { XINPUT_GAMEPAD_DPAD_DOWN,  40 },  // VK_DOWN
        { XINPUT_GAMEPAD_DPAD_LEFT,  37 },  // VK_LEFT
        { XINPUT_GAMEPAD_DPAD_RIGHT, 39 },  // VK_RIGHT
        { XINPUT_GAMEPAD_A,          13 },  // VK_RETURN (confirm)
        { XINPUT_GAMEPAD_B,          16 },  // VK_SHIFT (cancel)
        { XINPUT_GAMEPAD_X,          17 },  // VK_CONTROL
        { XINPUT_GAMEPAD_Y,          88 },  // 'X' key
        { XINPUT_GAMEPAD_START,      27 },  // VK_ESCAPE (menu)
        { XINPUT_GAMEPAD_BACK,       27 },  // VK_ESCAPE
    };
    xpadMappings = (XpadMapping*)malloc(sizeof(defaults));
    if (!xpadMappings) {
        xpadMappingCount = 0;
        DbgPrint("BS: default controller mapping allocation failed\n");
        return;
    }
    memcpy(xpadMappings, defaults, sizeof(defaults));
    xpadMappingCount = sizeof(defaults) / sizeof(XpadMapping);
}

static float xdkNormalizeStick(SHORT value) {
    return value < 0 ? (float)value / 32768.0f : (float)value / 32767.0f;
}

// Publish the physical XInput state through the modern RunnerGamepad API as
// well as the legacy keyboard mappings below. Edge transitions are generated
// once per simulated game frame, not once per outer polling iteration.
static void updateNativeGamepad(Runner* runner, const XINPUT_GAMEPAD* pad, bool connected) {
    if (!runner || !runner->gamepads) return;
    RunnerGamepadState* state = runner->gamepads;
    bool previous[GP_BUTTON_COUNT];
    for (int i = 0; i < GP_BUTTON_COUNT; i++)
        previous[i] = state->slots[0].buttonDown[i];

    RunnerGamepad_beginFrame(state);
    GamepadSlot* slot = &state->slots[0];
    for (int i = 0; i < GP_BUTTON_COUNT; i++)
        slot->buttonDownPrev[i] = previous[i];

    slot->connected = connected;
    if (connected && pad) {
        slot->jid = 0;
        strcpy(slot->description, "Xbox 360 Controller");
        strcpy(slot->guid, "xinput:0");

        bool down[GP_BUTTON_COUNT] = { false };
        float values[GP_BUTTON_COUNT] = { 0.0f };
        down[0] = (pad->wButtons & XINPUT_GAMEPAD_A) != 0;
        down[1] = (pad->wButtons & XINPUT_GAMEPAD_B) != 0;
        down[2] = (pad->wButtons & XINPUT_GAMEPAD_X) != 0;
        down[3] = (pad->wButtons & XINPUT_GAMEPAD_Y) != 0;
        down[6] = (pad->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        down[7] = (pad->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        down[8] = (pad->wButtons & XINPUT_GAMEPAD_BACK) != 0;
        down[9] = (pad->wButtons & XINPUT_GAMEPAD_START) != 0;
        down[10] = (pad->wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
        down[11] = (pad->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
        down[12] = (pad->wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
        down[13] = (pad->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
        down[14] = (pad->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
        down[15] = (pad->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;

        values[4] = (float)pad->bLeftTrigger / 255.0f;
        values[5] = (float)pad->bRightTrigger / 255.0f;
        down[4] = values[4] >= slot->triggerThreshold;
        down[5] = values[5] >= slot->triggerThreshold;
        for (int i = 0; i < GP_BUTTON_COUNT; i++) {
            if (i != 4 && i != 5) values[i] = down[i] ? 1.0f : 0.0f;
        }

        slot->axisValue[0] = xdkNormalizeStick(pad->sThumbLX);
        slot->axisValue[1] = -xdkNormalizeStick(pad->sThumbLY);
        slot->axisValue[2] = xdkNormalizeStick(pad->sThumbRX);
        slot->axisValue[3] = -xdkNormalizeStick(pad->sThumbRY);
        for (int i = 0; i < GP_BUTTON_COUNT; i++) {
            slot->buttonDown[i] = down[i];
            slot->buttonValue[i] = values[i];
            slot->buttonPressed[i] = down[i] && !previous[i];
            slot->buttonReleased[i] = !down[i] && previous[i];
        }
        state->connectedCount = 1;
    } else {
        memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
        memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
        memset(slot->axisValue, 0, sizeof(slot->axisValue));
        slot->description[0] = '\0';
        slot->guid[0] = '\0';
        state->connectedCount = 0;
    }
}

// Mirrors the modern platformSwapBuffers() hook: finish the D3D scene only
// after the GUI pass has been drawn, then present the completed back buffer.
static void platformSwapBuffers(D3D9Renderer* renderer) {
    if (!renderer || !renderer->pd3dDevice) return;
    IDirect3DDevice9* device = (IDirect3DDevice9*)renderer->pd3dDevice;

    if (renderer->sceneActive) {
        HRESULT hr = device->EndScene();
        renderer->sceneActive = false;
        if (FAILED(hr)) DbgPrint("BS: EndScene failed: 0x%08X\n", (unsigned)hr);
    }

    HRESULT hr = device->Present(NULL, NULL, NULL, NULL);
    if (FAILED(hr)) DbgPrint("BS: Present failed: 0x%08X\n", (unsigned)hr);
}

static void shutdownXdk(Runner* runner, VMContext* vm, Renderer* renderer,
                        XdkAudioSystem* xdkAudio, DataWin* dataWin,
                        XdkFileSystem* xdkFs, JsonValue* configRoot,
                        IDirect3DDevice9* device, IDirect3D9* d3d) {
    // Runner_free closes outstanding FileSystem handles and resets the VM,
    // so its dependencies must remain alive until it has completed.
    if (runner) Runner_free(runner);

    if (xdkAudio && xdkAudio->base.vtable && xdkAudio->base.vtable->destroy)
        xdkAudio->base.vtable->destroy((AudioSystem*)xdkAudio);
    if (renderer && renderer->vtable && renderer->vtable->destroy)
        renderer->vtable->destroy(renderer);
    if (vm) VM_free(vm);
    if (dataWin) DataWin_free(dataWin);
    if (configRoot) JsonReader_free(configRoot);
    free(xdkFs);

    if (device) device->Release();
    if (d3d) d3d->Release();
    free(xpadMappings);
    xpadMappings = NULL;
    xpadMappingCount = 0;
}

// ===[ Main Entry Point ]===

VOID __cdecl main() {
    DbgPrint("BS: main() entered\n");

    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) { DbgPrint("BS: FATAL: D3D create failed\n"); return; }
    DbgPrint("BS: D3D9 created\n");

    D3DPRESENT_PARAMETERS d3dpp;
    ZeroMemory(&d3dpp, sizeof(d3dpp));
    d3dpp.BackBufferWidth        = SCREEN_WIDTH;
    d3dpp.BackBufferHeight       = SCREEN_HEIGHT;
    d3dpp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    d3dpp.FrontBufferFormat      = D3DFMT_LE_X8R8G8B8;
    d3dpp.MultiSampleType        = D3DMULTISAMPLE_NONE;
    d3dpp.BackBufferCount        = 1;
    d3dpp.EnableAutoDepthStencil = TRUE;
    d3dpp.AutoDepthStencilFormat = D3DFMT_D24S8;
    d3dpp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    d3dpp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;

    IDirect3DDevice9* pd3dDevice = NULL;
    HRESULT hr = pD3D->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                    &d3dpp, &pd3dDevice);
    if (FAILED(hr)) {
        DbgPrint("BS: FATAL: CreateDevice failed\n");
        shutdownXdk(NULL, NULL, NULL, NULL, NULL, NULL, NULL, pd3dDevice, pD3D);
        return;
    }
    DbgPrint("BS: D3D device created\n");


    // ===[ Locate data.win ]===
    // On Xbox 360, game content is at game:\ (DVD/HDD) or d:\ (dev kit)
    const char* dataWinPath = NULL;

    // Try multiple paths. Use fopen() for detection since it goes through
    // the Xbox CRT which handles game:\ paths reliably on both hardware and emulators.
    static const char* searchPaths[] = {
        "game:\\data.win",
        "game:\\butterscotch\\data.win",
        "d:\\data.win",
        "d:\\butterscotch\\data.win",
        NULL,
    };

    DbgPrint("BS: searching for data.win\n");
    char msg[256];
    for (int i = 0; searchPaths[i]; i++) {
        DbgPrint("BS: trying %s\n", searchPaths[i]);

        FILE* testFile = fopen(searchPaths[i], "rb");
        if (testFile) {
            fclose(testFile);
            dataWinPath = searchPaths[i];
            DbgPrint("BS: found data.win at %s\n", dataWinPath);
            break;
        }
    }

    if (!dataWinPath) {
        DbgPrint("BS: FATAL: data.win not found\n");
        shutdownXdk(NULL, NULL, NULL, NULL, NULL, NULL, NULL, pd3dDevice, pD3D);
        return;
    }

    DbgPrint("BS: parsing data.win\n");

    DataWinParserOptions parseOpts;
    memset(&parseOpts, 0, sizeof(parseOpts));
    parseOpts.parseGen8 = true;  parseOpts.parseOptn = true;  parseOpts.parseLang = true;
    parseOpts.parseExtn = true;  parseOpts.parseSond = true;  parseOpts.parseAgrp = true;
    parseOpts.parseSprt = true;  parseOpts.parseBgnd = true;  parseOpts.parsePath = true;
    parseOpts.parseScpt = true;  parseOpts.parseGlob = true;  parseOpts.parseShdr = true;
    parseOpts.parseFont = true;  parseOpts.parseTmln = true;  parseOpts.parseObjt = true;
    parseOpts.parseRoom = true;  parseOpts.parseTgin = true;  parseOpts.parseTpag = true;  parseOpts.parseCode = true;
    parseOpts.parseVari = true;  parseOpts.parseFunc = true;  parseOpts.parseStrg = true;
    parseOpts.parseTxtr = true;  parseOpts.parseAudo = true;
    parseOpts.skipLoadingPreciseMasksForNonPreciseSprites = true;
    DataWin* dataWin = DataWin_parse(dataWinPath, parseOpts);

    if (!dataWin) {
        DbgPrint("BS: FATAL: DataWin_parse returned NULL\n");
        shutdownXdk(NULL, NULL, NULL, NULL, NULL, NULL, NULL, pd3dDevice, pD3D);
        return;
    }
    DbgPrint("BS: data.win parsed OK\n");

    DbgPrint("BS: game=%s\n", dataWin->gen8.displayName ? dataWin->gen8.displayName : "Unknown");

    // ===[ Load CONFIG.JSN (optional) ]===
    char configPath[512];
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        memcpy(configPath, dataWinPath, dirLen);
        sprintf(configPath + dirLen, "CONFIG.JSN");
    } else {
        strcpy(configPath, "CONFIG.JSN");
    }

    JsonValue* configRoot = NULL;
    HANDLE hConfig = CreateFileA(configPath, GENERIC_READ, FILE_SHARE_READ,
                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hConfig != INVALID_HANDLE_VALUE) {
        DWORD configSize = GetFileSize(hConfig, NULL);
        // CONFIG.JSN is optional and small. Reject invalid/oversized files
        // rather than allocating from an unchecked DWORD or parsing a partial read.
        if (configSize != INVALID_FILE_SIZE && configSize <= 1024U * 1024U) {
            char* configText = (char*)malloc((size_t)configSize + 1);
            if (configText) {
                DWORD bytesRead = 0;
                BOOL readOk = ReadFile(hConfig, configText, configSize, &bytesRead, NULL);
                if (readOk && bytesRead == configSize) {
                    configText[bytesRead] = '\0';
                    configRoot = JsonReader_parse(configText);
                    if (configRoot) OutputDebugStringA("Loaded CONFIG.JSN\n");
                } else {
                    OutputDebugStringA("Ignoring unreadable CONFIG.JSN\n");
                }
                free(configText);
            } else {
                OutputDebugStringA("Ignoring CONFIG.JSN: out of memory\n");
            }
        } else {
            OutputDebugStringA("Ignoring invalid or oversized CONFIG.JSN\n");
        }
        CloseHandle(hConfig);
    }

    // ===[ Create Subsystems ]===
    DbgPrint("BS: creating subsystems\n");
    XdkFileSystem* xdkFs = XdkFileSystem_create(dataWinPath);
    FileSystem* fileSystem = (FileSystem*)xdkFs;

    DbgPrint("BS: creating renderer\n");
    Renderer* renderer = D3D9Renderer_create(pd3dDevice);

    DbgPrint("BS: creating VM\n");
    VMContext* vm = VM_create(dataWin);

    DbgPrint("BS: creating audio system\n");
    XdkAudioSystem* xdkAudio = XdkAudioSystem_create();
    AudioSystem* audioSystem = (AudioSystem*)xdkAudio;

    if (!xdkFs || !renderer || !vm || !xdkAudio) {
        DbgPrint("BS: FATAL: subsystem allocation failed (fs=%d renderer=%d vm=%d audio=%d)\n",
                 xdkFs != NULL, renderer != NULL, vm != NULL, xdkAudio != NULL);
        shutdownXdk(NULL, vm, renderer, xdkAudio, dataWin, xdkFs, configRoot, pd3dDevice, pD3D);
        return;
    }

    DbgPrint("BS: creating modern runner\n");
    Runner* runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem, 0);
    if (!runner) {
        DbgPrint("BS: FATAL: Runner_create returned NULL\n");
        shutdownXdk(NULL, vm, renderer, xdkAudio, dataWin, xdkFs, configRoot, pd3dDevice, pD3D);
        return;
    }
    if (!((D3D9Renderer*)renderer)->initialized) {
        DbgPrint("BS: FATAL: D3D9 renderer initialization failed\n");
        shutdownXdk(runner, vm, renderer, xdkAudio, dataWin, xdkFs, configRoot, pd3dDevice, pD3D);
        return;
    }
    if (!xdkAudio->initialized) {
        DbgPrint("BS: FATAL: XAudio2 initialization failed\n");
        shutdownXdk(runner, vm, renderer, xdkAudio, dataWin, xdkFs, configRoot, pd3dDevice, pD3D);
        return;
    }

    // Parse CONFIG.JSN options
    if (configRoot) {
        JsonValue* disabledArr = JsonReader_getJsonValueByKey(configRoot, "disabledObjects");
        if (disabledArr && JsonReader_isArray(disabledArr)) {
            sh_new_strdup(runner->disabledObjects);
            int count = JsonReader_arrayLength(disabledArr);
            for (int i = 0; i < count; i++) {
                JsonValue* elem = JsonReader_getArrayElement(disabledArr, i);
                if (elem && JsonReader_isString(elem)) {
                    const char* name = JsonReader_getString(elem);
                    shput(runner->disabledObjects, name, 1);
                }
            }
        }

        JsonValue* mappingsObj = JsonReader_getJsonValueByKey(configRoot, "controllerMappings");
        if (mappingsObj && JsonReader_isObject(mappingsObj)) {
            int count = JsonReader_objectLength(mappingsObj);
            xpadMappings = count > 0 ? (XpadMapping*)malloc(sizeof(XpadMapping) * count) : NULL;
            xpadMappingCount = 0;
            for (int i = 0; xpadMappings && i < count; i++) {
                const char* btnStr = JsonReader_getJsonKeyByIndex(mappingsObj, i);
                JsonValue* gmlVal = JsonReader_getJsonValueByIndex(mappingsObj, i);
                if (!btnStr || !gmlVal) continue;
                xpadMappings[xpadMappingCount].xpadButton = (WORD)atoi(btnStr);
                xpadMappings[xpadMappingCount].gmlKey = (int32_t)JsonReader_getInt(gmlVal);
                xpadMappingCount++;
            }
            // Empty or unusable custom mappings fall back to the defaults below.
            if (xpadMappingCount == 0) { free(xpadMappings); xpadMappings = NULL; }
        }
    }

    if (!xpadMappings) setupDefaultMappings();

    // Runner_create owns renderer/audio initialization in the modern runtime.

    // Initialize first room
    DbgPrint("BS: init first room\n");
    Runner_initFirstRoom(runner);
    DbgPrint("BS: first room OK\n");

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t)gen8->defaultWindowWidth;
    int32_t gameH = (int32_t)gen8->defaultWindowHeight;
    DbgPrint("BS: gameW=%d gameH=%d screenW=%d screenH=%d\n", gameW, gameH, SCREEN_WIDTH, SCREEN_HEIGHT);

    DbgPrint("BS: entering modern main loop\n");

    LARGE_INTEGER freq, lastTime, currentTime;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0 ||
        !QueryPerformanceCounter(&lastTime)) {
        DbgPrint("BS: FATAL: performance counter unavailable\n");
        shutdownXdk(runner, vm, renderer, xdkAudio, dataWin, xdkFs, configRoot, pd3dDevice, pD3D);
        return;
    }
    double accumulator = 0.0;

    while (!runner->shouldExit) {
        if (!QueryPerformanceCounter(&currentTime)) {
            DbgPrint("BS: performance counter query failed; stopping main loop\n");
            break;
        }
        double deltaTime = (double)(currentTime.QuadPart - lastTime.QuadPart) / (double)freq.QuadPart;
        lastTime = currentTime;
        if (deltaTime < 0.0) deltaTime = 0.0;
        if (deltaTime > 0.25) deltaTime = 0.25;

        // Poll XInput. Treat a disconnected controller as neutral so keys do not stick.
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));
        WORD buttons = 0;
        bool controllerConnected = XInputGetState(0, &state) == ERROR_SUCCESS;
        if (controllerConnected) {
            buttons = state.Gamepad.wButtons;

            const SHORT deadzone = 16384;
            SHORT lx = state.Gamepad.sThumbLX;
            SHORT ly = state.Gamepad.sThumbLY;
            if (lx < -deadzone) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (lx >  deadzone) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (ly >  deadzone) buttons |= XINPUT_GAMEPAD_DPAD_UP;
            if (ly < -deadzone) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
            prevRightTrigger = state.Gamepad.bRightTrigger;
        } else {
            prevRightTrigger = 0;
        }

        for (int i = 0; i < xpadMappingCount; i++) {
            WORD mask = xpadMappings[i].xpadButton;
            int32_t gmlKey = xpadMappings[i].gmlKey;
            bool wasPressed = (prevButtons & mask) != 0;
            bool isPressed = (buttons & mask) != 0;

            if (isPressed && !wasPressed)
                RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
            else if (!isPressed && wasPressed)
                RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
        }
        prevButtons = buttons;

        bool speedCapRemoved = prevRightTrigger > 128;
        double gameSpeed = (double)Runner_getEffectiveGameSpeed(runner);
        double targetFrameTime = gameSpeed > 0.0 ? (1.0 / gameSpeed) : (1.0 / 60.0);

        accumulator += deltaTime;
        double maxAccumulator = targetFrameTime * 4.0;
        if (accumulator > maxAccumulator) accumulator = maxAccumulator;
        if (speedCapRemoved && targetFrameTime > accumulator) accumulator = targetFrameTime;

        int gameFramesRan = 0;
        while (accumulator >= targetFrameTime && !runner->shouldExit) {
            if (gameFramesRan > 0) RunnerKeyboard_beginFrame(runner->keyboard);
            updateNativeGamepad(runner, &state.Gamepad, controllerConnected);
            Runner_step(runner);
            accumulator -= targetFrameTime;
            gameFramesRan++;
        }

        // Render once after catch-up steps. This follows the modern renderer lifecycle:
        // draw-pre -> application surface/views -> draw-post -> GUI -> present.
        if (gameFramesRan > 0 && !runner->shouldExit) {
            gameW = runner->applicationWidth > 0 ? runner->applicationWidth : (int32_t)gen8->defaultWindowWidth;
            gameH = runner->applicationHeight > 0 ? runner->applicationHeight : (int32_t)gen8->defaultWindowHeight;

            Runner_beginFrame(runner, gameW, gameH,
                              SCREEN_WIDTH, SCREEN_HEIGHT,
                              SCREEN_WIDTH, SCREEN_HEIGHT);
            // The XDK backend currently aliases application_surface to the host
            // framebuffer. Clear it once before Draw Pre, then preserve that
            // pass when Runner_drawViews performs its normal clear callback.
            renderer->vtable->clearScreen(renderer,
                runner->drawBackgroundColor ? runner->backgroundColor : 0, 1.0f);
            Runner_drawPre(runner, SCREEN_WIDTH, SCREEN_HEIGHT);
            // Suppress only Runner_drawViews' own initial clear, after any
            // intentional clear calls made by Draw Pre have already run.
            ((D3D9Renderer*)renderer)->skipNextClearScreen = true;
            Runner_drawViews(runner, gameW, gameH, false);
            renderer->vtable->endFrameInit(renderer);
            Runner_drawPost(runner, SCREEN_WIDTH, SCREEN_HEIGHT);
            renderer->vtable->endFrameEnd(renderer);
            Runner_drawGUI(runner, SCREEN_WIDTH, SCREEN_HEIGHT, gameW, gameH);
            platformSwapBuffers((D3D9Renderer*)renderer);

            Runner_handlePendingRoomChange(runner);
            RunnerKeyboard_beginFrame(runner->keyboard);
        }

        if (audioSystem && audioSystem->vtable && audioSystem->vtable->update) {
            float audioDelta = (float)deltaTime;
            if (audioDelta > 0.1f) audioDelta = 0.1f;
            audioSystem->vtable->update(audioSystem, audioDelta);
        }

        // Avoid a full-speed busy loop when the room speed is low.
        if (!speedCapRemoved && accumulator < targetFrameTime) {
            DWORD sleepMs = (DWORD)((targetFrameTime - accumulator) * 1000.0);
            if (sleepMs > 0) Sleep(sleepMs > 2 ? sleepMs - 1 : sleepMs);
        }
    }

    // ===[ Cleanup ]===
    DbgPrint("BS: shutting down runner\n");
    shutdownXdk(runner, vm, renderer, xdkAudio, dataWin, xdkFs, configRoot, pd3dDevice, pD3D);
}
