# Building with XDK 21256.3 and running on an RGH console (Aurora)

Step-by-step for the first real XDK build. Background and licensing notes are in
[XDK_BUILD_AND_TESTING.md](XDK_BUILD_AND_TESTING.md); nothing from the XDK belongs in this repository.

## Verification status

| Level | Status |
|---|---|
| Source-level checks (`tools/xdk/check_xdk_sources.py`) | Passing. Checks vtable coverage, the `.vcxproj` source list, Debug/Release library lists, include directories and unconditional `#include "..."` resolution. Runs in CI. |
| Renderer transform math (`tools/xdk/test_d3d9_transform.c`) | Passing on the host. Mutation-checked: breaking the flip, the letterbox or the multiply order makes it fail. Runs in CI. |
| XDK environment check (`build-xdk.ps1 -CheckOnly`) | Passing with VS2010 + SP1 + XDK 21256.3 installed (and correctly lists what is missing without them). |
| Xbox 360 XDK compilation / XEX | **Done.** The smoke XEX and the `Release` Butterscotch XEX build with XDK 21256.3 (`_MSC_VER=1600`): 0 errors, no uninitialised-variable or missing-return warnings. `Debug` has not been built. |
| Host C++ syntax pass of the core | Passing for all 55 `.c` translation units the XDK project compiles. Clang compiles them as C++ with `compat_msvc.h` forced and `_XBOX`/`_WIN32` defined, against MinGW headers. This catches C-only syntax (`goto` past initialisers, designated initialisers, VLAs). It does **not** prove MSVC 2010 compatibility, and the four XDK `.cpp` files were not compiled. |
| Console execution | **Not done yet.** |

## 1. One-time setup (Windows PC or VM)

1. Install **Visual Studio 2010** with Visual C++ and SP1. The XDK integrates into the VS2010 MSBuild toolset, so install VS2010 first.
2. Install **XDK 21256.3**. Include the Visual Studio integration and the build tools. The installer sets the `XEDK` environment variable.
3. Open a new terminal and check the install:

```bash
powershell -ExecutionPolicy Bypass -File tools\xdk\build-xdk.ps1 -CheckOnly
```

It prints `ok` or `MISSING` for the compiler, `imagexex.exe`, headers, every library the project links, MSBuild 4.0, VS2010 and the MSBuild `Xbox 360` platform. If the XDK lives somewhere else, add `-XdkDir "D:\path\to\XDK"` or set `XEDK`.

## 2. Build and run the smoke test first

The smoke test is a small XEX built with the same compiler settings, forced `compat_msvc.h` and XDK libraries as the real port, but without the runner. If this fails, the problem is the toolchain or install, not Butterscotch.

```bash
powershell -ExecutionPolicy Bypass -File tools\xdk\build-xdk.ps1 -Target smoke -Package
```

The output is `dist\smoke\default.xex`. The full log is `build-xbox360-xdk\smoke-Release.log`.

**Upload with Aurora's FTP server.** In Aurora, turn FTP on in its settings and note the console IP and the FTP user name and password it shows. Then upload with Windows' built-in `curl` (replace the placeholders):

```bash
curl -T dist\smoke\default.xex --ftp-create-dirs -u USER:PASS ftp://CONSOLE_IP/Hdd1/Homebrew/ButterscotchSmoke/
```

FileZilla works too: connect to the console IP on port 21 and copy the file to `Hdd1/Homebrew/ButterscotchSmoke/`.

**Launch it.** Either add `Hdd1:\Homebrew` as a scan path in Aurora's content settings and rescan, or open `default.xex` from Aurora's file browser.

**What you should see:** the screen slowly cycles through colours. A freezes or unfreezes the colour. Back returns to Aurora. The XEX also writes `smoke.txt` next to itself. Download it:

```bash
curl -u USER:PASS ftp://CONSOLE_IP/Hdd1/Homebrew/ButterscotchSmoke/smoke.txt
```

It must report `big_endian=1`, `d3d9=ok` and `xaudio2=ok`. `isnan(0/0)=1 isfinite(1)=1 log2(8)=3.0 exp2(3)=8.0` confirms the C99 polyfills work. Send the whole file back, because `_MSC_VER` and `_WIN32` decide which compatibility paths the port takes.

## 3. Build Butterscotch

Use **Release** on RGH. Debug links `xbdm.lib`, which needs the xbdm plugin loaded through DashLaunch.

```bash
powershell -ExecutionPolicy Bypass -File tools\xdk\build-xdk.ps1 -Configuration Release -Package -DataWin "C:\path\to\game\data.win"
```

This stages `dist\butterscotch\default.xex` and `data.win`. Upload the whole folder. Include any `audiogroup*.dat`, external `.ogg` files and an optional `CONFIG.JSN` from the game folder:

```bash
curl -T dist\butterscotch\default.xex --ftp-create-dirs -u USER:PASS ftp://CONSOLE_IP/Hdd1/Homebrew/Butterscotch/
```

```bash
curl -T dist\butterscotch\data.win -u USER:PASS ftp://CONSOLE_IP/Hdd1/Homebrew/Butterscotch/
```

The runner looks for `game:\data.win`, which is the folder that holds the XEX.

The equivalent CMake entry point (Windows only) is:

```bash
cmake -S . -B build-xdk -DPLATFORM=xbox360-xdk
```

```bash
cmake --build build-xdk
```

## 4. When the build fails

The script prints the first 15 `error` lines. The full log is `build-xbox360-xdk\butterscotch-Release.log`. Report the **first** error, because later errors are often consequences. These are the places most likely to need a follow-up, none of which could be compiled before this guide was written:

- **C-as-C++ issues in the modern core.** Every `.c` file is compiled as C++ by a VS2010-era compiler. Expect some implicit `void*` and enum conversions, `goto` jumping past initialisers, or C99 library calls that `compat_msvc.h` does not cover yet. Fix them in `compat_msvc.h` or with a cast at the call site.
- **CRT headers.** `overlay_file_system.c` includes `<sys/stat.h>` and `<direct.h>`. If the Xbox CRT lacks one, guard it with `!defined(_XBOX)` the same way `data_win.c`, `utils.h` and `runner.c` are guarded.
- **`windows.h`.** The XDK compiler defines `_WIN32`, so shared code includes `<windows.h>`. `src/xbox360-xdk/windows.h` forwards that to `<xtl.h>`. If a desktop-only Win32 call then fails to compile, guard it with `!defined(_XBOX)`.

## 5. First console run checklist

Check these in order, and note the first one that fails:

1. Boots to the first room, with no hang on a black screen.
2. Sprites and text sit at the right positions and scale. The renderer now maps every vertex through the GML view/projection/world matrices.
3. Rotated views (`view_angle`), `camera_apply` and sequences, which go through the new `applyProjection` and `setMatrix` paths.
4. GUI layer: positioned correctly at 1280x720 and not stretched.
5. `draw_text_colour` gradients and `draw_text_ext` line spacing.
6. Controller: D-pad, A/B and the left stick map to keys. `gamepad_*` reads the native pad.
7. Audio: SFX and music, looping, and pause on the guide button.
8. Room transitions, then a 10-minute session to check memory stability.

## How the XDK renderer maps coordinates

The D3D9 backend transforms vertices on the CPU: `screen = viewport(NDC) × projection × view × world`, using the same `gmlMatrices` as upstream. The math is in `src/xbox360-xdk/d3d9_transform.h` and is host-tested. These conventions follow upstream `gl_common.c` and `gl_renderer.c` at `0c94bfb`:

- **Views:** `beginView` applies the view camera's own matrices, which include `view_angle`, then lets an active `d3d_start` override them. The port is letterboxed into the 1280x720 back buffer.
- **GUI, Draw Post, Draw Pre:** these use upstream's GUI camera (`Orthographic(guiW, guiH)` and a centred `LookAt`), letterboxed through the viewport. Upstream builds these matrices with the top edge at clip −1 and relies on GL's `flipClipY`, so the backend flips Y while a GUI pass targets the back buffer. The world matrix is reset for the pass and restored afterwards, as upstream does.
- **Draw Pre** targets `application_surface`, which is the back buffer here, so it is flipped like the GUI. Upstream draws it into an FBO without the flip. That probably renders Draw Pre content upside down in upstream GL, so this is a deliberate difference.

## Known gaps (not implemented in the XDK renderer)

- **User surfaces.** `surface_create` returns -1. `application_surface` is the back buffer itself, so effects that draw to or from surfaces are missing. This needs EDRAM render targets plus `Resolve` and must be tested on hardware.
- **Custom shaders.** `shader_set` and uniforms are no-ops (`shaders_supported` is false). A fixed sprite shader is always used.
- **Vertex buffers, fog, depth state and `draw_tile`** are left NULL. The core checks for NULL before calling them, so they are skipped rather than crashing.
- **Sprites from surfaces.** `sprite_create_from_surface` returns -1.
- **3D perspective projections** draw as orthographic, because there is no clip-space w divide.
