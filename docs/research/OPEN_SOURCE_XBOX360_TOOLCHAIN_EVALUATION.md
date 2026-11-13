# Open-source Xbox 360 toolchain evaluation

**Research date:** 2026-11-13  
**Target:** `Viniqpp/Butterscotch-360-2`  
**Branch:** `refresh2/modern-upstream-migration`  
**Purpose:** Determine whether the project can produce an RGH-loadable XEX without access to Microsoft's proprietary Xbox 360 XDK.

## Executive result

The first candidate, **OXDK**, does not solve the missing-SDK problem: its Xbox 360 target still requires the user to supply an extracted Xbox 360 XDK, including `lib/xbox/xboxkrnl.lib`. It replaces the Windows/MSVC build glue with a Xenon-ABI-patched Clang/LLD toolchain, but it is not an XDK replacement.

A substantially better candidate is **xsdk** ([xenosworks/xsdk](https://github.com/xenosworks/xsdk)). It is a from-scratch, open-source Xbox 360 homebrew SDK that builds XEX2 titles for the retail kernel and RGH/JTAG dashboards without Microsoft tooling or Microsoft SDK files. Its documented host setup is Windows 10/11 with WSL 2 or Debian/Linux, and its getting-started guide specifically uses Debian 13 and Aurora/FTP for deployment.

**Recommendation:** evaluate a separate `xsdk` platform backend for Butterscotch rather than trying to compile the existing XDK backend unchanged. Keep the existing `xbox360-xdk` backend as a separate reference path. Do not mix the two toolchains or claim the current XDK backend builds under xsdk.

## Candidate comparison

| Candidate | Requires Microsoft XDK? | Host | Assessment |
|---|---:|---|---|
| Historical Xbox 360 XDK + VS integration | Yes | Windows, matching legacy VS/XDK toolset | Reference build path, but unavailable in this work environment |
| OXDK | **Yes** | Linux/macOS | Not a solution to the missing XDK; useful only if a legitimate XDK copy is available |
| xsdk | **No** | Debian/Linux or Windows 10/11 + WSL 2 | Best candidate for an independent RGH homebrew backend |
| libxenon | No | Linux | Older bare-metal path; less attractive than xsdk for a retail-kernel/Aurora title |

## OXDK verification

OXDK's own Xbox 360 guide says it produces XEX2 for RGH/devkit consoles but requires an extracted Xbox 360 XDK. It expects the directory containing `lib/xbox/xboxkrnl.lib`, then translates XDK libraries for its patched LLVM toolchain. Therefore OXDK cannot currently provide a complete build in an environment with no authorized XDK installation.

Source: [OXDK Xbox 360 build guide](https://github.com/MrMilenko/OXDK/blob/main/docs/xbox360.md).

## xsdk findings

The xsdk README and getting-started guide document:

- XEX2 output accepted by the retail kernel/dashboard on RGH/JTAG consoles.
- No Microsoft tooling or leaked SDK material in the project.
- C17/newlib runtime and a custom GCC 15.3, binutils 2.47 and newlib 4.6 toolchain for `powerpc-xsdk-elf`.
- A custom CMake toolchain file at `cmake/xsdk.cmake`.
- GPU command submission, render targets, resolves, textures, shaders, blending, depth/stencil, indexed/instanced draws and presentation.
- Audio output through a six-channel, 48 kHz API.
- Logging to a per-title file and optional network forwarding.
- FTP deployment and launch through a dashboard such as Aurora.
- Windows 10/11 + WSL 2 or Debian/Linux as supported build hosts; Debian 13 is the documented reference environment.
- Approximately 10 GB free space for the documented toolchain setup (about 1.5 GB installed and 7 GB temporary build space).

The README reports hardware-tested milestones through milestone 58 as of 2026-10-21, with specific caveats recorded in its scope/parity documentation. This is the project's own status statement, not an independent hardware verification by this research session.

Sources:
- [xsdk README](https://github.com/xenosworks/xsdk/)
- [xsdk getting started](https://github.com/xenosworks/xsdk/blob/main/docs/GETTING_STARTED.md)
- [xsdk GPU API](https://github.com/xenosworks/xsdk/blob/main/include/xsdk/gpu.h)
- [xsdk audio API](https://github.com/xenosworks/xsdk/blob/main/include/xsdk/audio.h)

## Compatibility with this Butterscotch branch

The current branch's native project is not directly buildable with xsdk. The existing `Butterscotch360.vcxproj` is an XDK project and links XDK-specific libraries such as D3D9, XGraphics, XAudio2, XAPI and Xbox kernel libraries. Its platform sources include:

- `src/xbox360-xdk/main.cpp`
- `src/xbox360-xdk/d3d9_renderer.cpp`
- `src/xbox360-xdk/xaudio2_audio.cpp`
- `src/xbox360-xdk/xdk_file_system.cpp`

Those files use XDK headers and APIs (`xtl.h`, `d3d9.h`, `xgraphics.h`, `xaudio2.h`, Win32/XDK file handles). They must not be compiled as though xsdk provided the same ABI or API.

### Potentially reusable pieces

- The modern runner/core sources are C and can be compiled with a cross toolchain, subject to checking the actual compiler flags and C-library assumptions.
- The modern renderer interface and `src/noop_renderer.c` provide a useful contract/reference for a new renderer implementation.
- The existing D3D9 renderer's GameMaker drawing semantics, texture-page handling, blend-mode mapping and sprite batching are useful behavioral references, but the D3D9 calls themselves are not reusable as xsdk calls.
- The current XDK renderer uses static HLSL vertex/pixel shader source strings. xsdk documents Shader Model 3 HLSL compilation through its glslang-xenon + xost toolchain, so an offline shader-generation path appears plausible. This needs a minimal shader proof before committing to the design.
- The XSDK ecosystem includes FAudio-xenon, described by its maintainers as XAudio2 API compatibility, X3DAudio and XAPO effects mixed on-console. This may reduce audio porting work, but its exact API/version/build compatibility with Butterscotch must be checked.

### Required new backend work

1. Add a separate platform directory (provisionally `src/xsdk/`) and CMake configuration for `PLATFORM=xsdk`; do not put xsdk code in `src/xbox360-xdk/`.
2. Implement the modern Runner renderer vtable against xsdk's GPU/texture/shader/present APIs. This is a real renderer port, not a vtable-padding exercise.
3. Prove shader compilation and texture-page upload early. The current XDK code's static HLSL pair is a useful first target; dynamic shader needs must be checked against the actual game data path.
4. Implement audio either directly over `xsdk_audio_*` or through a compatible FAudio-xenon layer. Preserve the modern 31-entry AudioSystem contract and explicitly report unsupported behavior.
5. Implement the modern filesystem contract using xsdk/newlib storage facilities. Verify paths, directory enumeration, rename/delete semantics and the game's expected writable location on the console.
6. Implement controller polling, monotonic timing, startup/shutdown, display presentation and logging with xsdk APIs.
7. Build a tiny XEX smoke target first (startup, log, controller, clear/present, audio tone, file read/write), deploy it through Aurora FTP, then integrate the Butterscotch core.
8. Only after the smoke target works should the full modern renderer and game be brought up in stages.

### Important existing blocker remains

The current XDK D3D9 renderer is still not a complete implementation of the modern renderer vtable; `Runner_create` validates required callbacks. An xsdk backend will need to implement that same modern contract. Switching toolchains does not remove the renderer migration work.

## Can this work session install xsdk?

The current execution container is Debian 13 with CMake 3.31.6, Python 3.13.5 and about 30 GB free, so its OS, CMake/Python versions and disk budget are compatible with the documented xsdk setup. It has Clang 17 and GCC 14.2, but xsdk requires building its own patched GCC 15 toolchain; the installed stock compiler is not a substitute.

**The container currently cannot resolve external hosts (`github.com` DNS lookup fails), so it cannot clone the xsdk sources or download the compiler sources.** I therefore could not install or build xsdk here. This is an environment network restriction, not an xsdk build failure. The repository files and build requirements were inspected through GitHub's connected source access.

The user's Windows 11 PC can follow xsdk's documented WSL 2 + Debian 13 route, with Aurora on the same network for FTP deployment. No XDK installation is required for that route.

## Decision

- Do not spend time trying to install OXDK without an XDK; it has the same missing prerequisite.
- Keep the XDK project isolated and unverified.
- Prioritize a minimal xsdk smoke title and a separate Butterscotch xsdk backend as the practical no-XDK route.
- Do not call the project "ported" or "working" until the generated XEX has been deployed and tested on the user's RGH console.
