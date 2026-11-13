# Xbox 360 XDK Build Environment Research

**Research date:** 2026-11-13  
**Target repository:** [Viniqpp/Butterscotch-360-2](https://github.com/Viniqpp/Butterscotch-360-2)  
**Target branch:** `refresh2/modern-upstream-migration`  
**Scope:** build and validation strategy only. This report does not claim that the XDK target has been compiled or run.

## Executive recommendation

Keep the current Microsoft Xbox 360 XDK target as the reference implementation and first try to reproduce its intended Windows build environment. The project is explicitly configured as an Xbox 360 XDK Visual Studio project and links XDK-specific D3D9, XAudio2, XInput/XAPI, kernel and debug libraries. Neither the modern Xbox GDK nor a generic desktop MSVC toolchain is a substitute.

Recommended order:

1. **Primary/reference path:** Windows 7-era-compatible Windows environment (preferably an isolated VM or dedicated machine), Visual Studio 2010/MSBuild 4.0-era Xbox 360 project integration, and a legitimately licensed Xbox 360 XDK installation whose platform toolset matches the project. Use the XDK version already associated with the project if it can be identified from its original build environment; otherwise validate a known XDK installation against the project's Debug and Release configurations before changing project settings.
2. **Modern-host experimental path:** evaluate [OXDK](https://github.com/MrMilenko/OXDK), which uses a Xenon-ABI-patched Clang/LLD toolchain to build XEX2 files against an XDK supplied by the user. It is a build-system alternative, not an SDK replacement. Its documented restrictions make it a promising experiment, not a verified drop-in build for this repository.
3. **CI:** run the existing Linux headless/no-op CMake build, source/static checks, and portable unit tests on every change. Keep an XDK-specific build as a separate, opt-in/self-hosted job if a legally usable XDK host becomes available. Never label the portable job an Xbox build.
4. **Runtime validation:** smoke-test XEX output in Xenia Canary, then verify on an RGH/JTAG console or genuine development kit. Emulator success is not hardware proof.

**Important distinction:** Xbox GDK/GDKX targets modern Xbox generations and does not support Xbox 360 development. Do not install or configure GDK expecting it to build this XDK project. Microsoft says this directly in its GDK announcement/FAQ ([Microsoft, “Meet the GDK”](https://developer.microsoft.com/en-us/games/articles/2021/07/meet-the-microsoft-game-developer-kit-gdk/)).

## 1. Repositories and exact source baselines inspected

The requested branch exists. Its tip was resolved by asking GitHub for the commit named by the branch:

| Repository | Exact commit inspected | Notes |
|---|---|---|
| [Viniqpp/Butterscotch-360-2](https://github.com/Viniqpp/Butterscotch-360-2) | `39fc2d4b9c5163f7d1a931a743e8767991974a6c` | Tip of `refresh2/modern-upstream-migration` at research time; includes the migration audit and latest XDK integration notes. |
| [ButterscotchRunner/Butterscotch](https://github.com/ButterscotchRunner/Butterscotch) | `0c94bfb05e3cb4b8946a38f8085747035dd37f94` | Modern upstream baseline recorded in the migration notes; current upstream may advance independently. |
| [ceilingtilefan/Butterscotch-360](https://github.com/ceilingtilefan/Butterscotch-360) | `1fd7659a4b95e0ce9837c3162de976cfc9ceec5a` | Original Xbox 360 port baseline recorded in the migration notes and present in the fork's history. |

The fork's [migration audit](../XBOX360_REFRESH2_MIGRATION.md) states that the modern runner has substantially expanded its platform contracts: renderer 16 to 81 entries, audio 21 to 31, and filesystem 5 to 20. It also records that the current D3D9 renderer still implements the legacy renderer contract and is not yet compatible with modern `Runner_create`'s renderer-vtable validation. The XDK target therefore has a source-level blocker independent of toolchain setup.

### Project-file facts observed at the inspected fork commit

The root `Butterscotch360.vcxproj` has:

- `ToolsVersion="4.0"` and `_ProjectFileVersion 10.0.30319.1`, consistent with the VS2010/MSBuild 4.0 project generation era. This is a project-format clue, not proof that any arbitrary VS2010 installation has the Xbox platform targets installed.
- Configurations named `Debug|Xbox 360` and `Release|Xbox 360`.
- `ForcedIncludeFiles=compat_msvc.h` and `CompileAs=CompileAsCpp` for the project sources.
- `ExceptionHandling=false` and `RuntimeTypeInfo=false`.
- Debug links: `d3d9d.lib`, `d3dx9d.lib`, `xgraphicsd.lib`, `xapilibd.lib`, `xaudiod2.lib`, `xmcored.lib`, `xboxkrnl.lib`, `xbdm.lib`.
- Release links: `d3d9.lib`, `d3dx9.lib`, `xgraphics.lib`, `xapilib.lib`, `xaudio2.lib`, `xmcore.lib`, `xboxkrnl.lib`.
- A 1 MiB stack reserve, Xbox-specific preprocessor definitions, and deployment metadata targeting `xe:\\Butterscotch`.

The current XDK project source list includes modern core C sources, the D3D9/XAudio2/filesystem adapters, image/font/FM​​OD/physics support and vendored libraries. The project currently compiles the listed C files as C++, so C++ compiler compatibility is material.

**Not verified:** the exact XDK version used to produce the original port's last working binary. The project file does not identify an XDK release number.

## 2. Historical Xbox 360 XDK and Visual Studio requirements

### Verified historical facts

- The Xbox 360 XDK is a proprietary Microsoft development kit, separate from the later Xbox GDK.
- A third-party build-system compatibility note lists XDK releases `2.0.6274.0` (July 2007), `2.0.7645.1` (November 2008) and `2.0.8276.0` (March 2009), and says the Xbox project Visual Studio add-ins work with Visual Studio 2005 and 2008. Source: [CloudBees KBEA-00013](https://docs.cloudbees.com/docs/cloudbees-build-acceleration-kb/latest/cloudbees-build-acceleration-kb/kbea-00013-xbox-360-development-kit-versions).
- A separate Xbox 360 development guide documents Visual Studio 2010 as the environment for the XDK templates and debugging integration. Source: [Unofficial Xbox 360 Development Guide — Development Environment](https://xb360.dev/development-environment/).
- The inspected project uses MSBuild `ToolsVersion=4.0`, which Microsoft documents as the project toolset format used by Visual Studio 2010 and 2012. Opening a VS2008 project in VS2010 updates it to ToolsVersion 4.0. Source: [Microsoft Learn — MSBuild Toolset (ToolsVersion)](https://learn.microsoft.com/en-us/visualstudio/msbuild/msbuild-toolset-toolsversion).
- Community XDK project build instructions commonly describe VS2010 plus the official Xbox 360 XDK. For example: [X-Store compiling notes](https://github.com/951261/X-Store/blob/main/COMPILING.md) and [Xbox 360 development tools](https://pilosophos.com/writing/xbox-360-development-tools).

### Practical interpretation (not a Microsoft support guarantee)

The VS2005/2008 compatibility note and VS2010-era project format are not necessarily contradictory: they refer to different XDK generations and integration generations. The safest procedure is to match the XDK's installed Xbox platform targets and compiler to the project, rather than assuming that the newest Visual Studio can build it.

For this repository, **VS2010 with the matching Xbox 360 platform toolset is the first environment to investigate**, because the project was generated in the VS2010/MSBuild 4.0 era. Keep a separate VS2008 environment as a fallback only if the particular XDK installation's integration requires it. Do not blindly retarget the project to VS2019/2022.

An XDK release often cited in community workflows is `2.0.21256.3` (2013-era); this research did not establish that it was the version used by this repository or that it is the best match for its project. Treat it as a candidate to verify, not a hard requirement.

### Recommended host setup

- Use an isolated Windows VM or dedicated Windows machine. Windows 7 is often cited in historical guides; a later Windows host may work, but legacy installer, driver, MSBuild and debugger behavior must be tested rather than assumed.
- Install the Visual Studio version required by the XDK integration first, then install the XDK with its full Xbox 360 build/deployment components.
- Confirm that the Xbox 360 platform appears in the VS project configuration manager and that the XDK's platform toolset files are installed under the expected MSBuild tree.
- Preserve an immutable copy/snapshot of the working environment and record the XDK version, compiler version, MSBuild version, environment variables and hashes of installed tool binaries.
- Keep the source checkout outside protected system directories. Do not mix modern desktop Windows SDK headers/libraries with the Xbox 360 platform include and library paths.

## 3. Compiler and language compatibility

### What the current project expects

The current project file's explicit settings are the best starting point:

| Setting | Debug | Release |
|---|---|---|
| Compile language | C++ (`CompileAsCpp`) | C++ (`CompileAsCpp`) |
| Forced include | `compat_msvc.h` | `compat_msvc.h` |
| Exceptions | Off | Off |
| RTTI | Off | Off |
| Runtime | Multithreaded Debug | Multithreaded |
| Optimization | Disabled | Full, size preference |
| Defines | `_DEBUG;_XBOX;XBOX` plus feature flags | `NDEBUG;_XBOX;XBOX` plus feature flags |
| Parallel compile | `/MP` | `/MP` |
| Stack reserve | 1 MiB | project default / XDK setting |
| Debug info | Enabled | not explicitly enabled in the project file |

Preserve the project's Xbox-specific configuration and XDK library names. Do not substitute desktop `d3d9.lib`, desktop `xaudio2.lib`, or a modern Windows SDK for Xbox libraries.

### Likely MSVC compatibility issues

The modern upstream project is written as C and sets C99 in its CMake build. The Xbox project currently compiles the selected C files as C++, likely to accommodate the legacy MSVC compatibility shim and Xbox headers. This creates two distinct compatibility surfaces:

1. **C-to-C++ source compatibility:** C-only constructs may fail when compiled as C++, including designated initializers, compound literals, implicit `void*` conversions, variable-length arrays, and C-specific linkage/keyword assumptions. Audit actual occurrences rather than assuming every example is present.
2. **Old C++ compiler support:** VS2010-era MSVC predates full C++11 support and modern standard-library facilities. Features such as `nullptr`, lambdas, range-based loops, `auto`, `constexpr`, `enum class`, rvalue references and newer library APIs must be checked against the exact compiler build. The Xbox compiler may also differ from desktop MSVC.

Microsoft's current conformance table records that most C++11 language support arrived in VS2015, while C11/C17 compiler support requires VS2019 16.8 or later. The C compiler's default mode is older C89 with extensions and does not provide full C99 conformance. Sources: [Microsoft C/C++ language conformance](https://learn.microsoft.com/en-us/cpp/visual-cpp-language-conformance) and [MSVC `/std` documentation](https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version).

Additional likely issues to investigate:

- Modern upstream code may use C99 features that do not survive the current C++ compilation mode.
- Newer headers may rely on standard-library APIs unavailable in the Xbox CRT/STL.
- MSVC-specific `long`/integer-width assumptions, packing/alignment, calling conventions and variadic handling can differ from GCC/Clang.
- The project disables exceptions and RTTI; any newly imported source that requires them will fail or require a deliberate design change.
- The Xbox compiler/linker has its own ABI, import libraries, XEX packaging and debug/deployment integration. A successful desktop compile is not an Xbox link.
- XDK headers may depend on Microsoft extensions, macros and case-insensitive include behavior.
- Debug and Release use different XDK library variants; mixing the variants can produce unresolved symbols or invalid runtime behavior.

### Compile-setting policy

Start with the existing project settings. Do not globally enable modern language modes, exceptions or RTTI just to make an error disappear. For each error, determine whether it is a source-language issue, an XDK header issue, a missing XDK library, or a genuine platform API incompatibility. Prefer narrow compatibility wrappers and source-specific settings. Keep the target ABI and Xbox calling conventions unchanged.

## 4. OpenXDK, OXDK, libxenon and the newer xsdk

These names refer to different things.

### OpenXDK

The historic OpenXDK project targets the **original Xbox**, not Xbox 360. It is not a compatible SDK replacement for this XDK project. Do not confuse it with OXDK below.

### OXDK (promising XDK cross-build experiment)

[MrMilenko/OXDK](https://github.com/MrMilenko/OXDK) is a modern cross-platform build system for original Xbox and Xbox 360 XDK projects. It is **not** the Microsoft SDK and does not ship Microsoft's code: the user supplies a legitimate extracted XDK. Its Xbox 360 path builds XEX2 using a Xenon-ABI-patched Clang/LLD toolchain. Its documentation explicitly covers D3D9 libraries, XDK library linking, XEX wrapping and XBDM deployment.

Useful documented details:

- Build its patched compiler with `./oxdk build-llvm`; the project says this requires roughly 20 GB for the build.
- Point `XDK_DIR` at an extracted XDK directory containing `lib/xbox/xboxkrnl.lib`, then run `./oxdk doctor`.
- Run the `abitest` sample first; it checks ABI-sensitive formatting, including 64-bit integers and doubles.
- The Xbox 360 make integration translates XDK libraries to ELF, resolves XEX imports and packages XEX2.
- The project documents limitations: compile with `-fno-exceptions`; `try`/`throw` are unavailable; libc++ threads are disabled; XDK C++ library binaries cannot be linked as ordinary Clang C++ because of incompatible C++ name mangling. Header-only templates/inline code can work, while iostreams need a separately built libc++.

Sources: [OXDK overview](https://github.com/MrMilenko/OXDK), [Xbox 360 guide](https://github.com/MrMilenko/OXDK/blob/main/docs/xbox360.md).

**Assessment for Butterscotch 360:** OXDK is the strongest non-Microsoft-host build experiment because it targets the actual XDK libraries and ABI. It is not yet verified against this codebase. The current XDK project compiles all selected sources as C++, uses XDK D3D9 and XAudio2, and includes a compatibility header. Before attempting a full port, create a throwaway OXDK sample that includes the same XDK headers, links the same D3D9/XAudio2 libraries, and calls the same APIs. Then build the current adapters one at a time. Confirm that the XDK release's XAudio2 library is linkable under OXDK and that no required C++ CRT features are missing. A compile/link success is still not proof of runtime correctness.

### libxenon

[libxenon](https://github.com/X360Tools/libxenon) is a separate bare-metal homebrew library/toolchain for Xbox 360. It uses a PowerPC cross compiler and its own runtime/library APIs rather than the Microsoft XDK. It can build Xbox 360 homebrew, but it is not source- or binary-compatible with an XDK application.

A libxenon port would need to replace or adapt at least:

- D3D9 renderer calls with a libxenon graphics path (or another compatible graphics implementation).
- XAudio2 voice/device setup with libxenon's audio facilities.
- XDK filesystem, input, timers, threading and kernel calls.
- XDK project/build metadata, linker imports, XEX packaging and debugging/deployment assumptions.

**Assessment:** not a realistic drop-in way to build the current project. It is a possible long-term alternative backend only if the team deliberately chooses to maintain a separate platform port and accepts substantial renderer/audio/platform work.

### xsdk (worth tracking as a newer alternative)

[xenosworks/xsdk](https://github.com/xenosworks/xsdk) is a newer, from-scratch Xbox 360 homebrew SDK based on public reverse engineering. It documents a GCC 15/newlib toolchain, a native XEX2 packer, kernel/system APIs, six-channel audio and a GPU layer with shader support. It explicitly avoids Microsoft SDK binaries and leaked SDK material. Its README reports console-tested milestones through October 2026, but that is the project's own status report, not independent validation.

**Assessment:** more capable and actively documented than a simplistic libxenon comparison might suggest, but still not a drop-in XDK backend. It would require porting Butterscotch's platform layer to xsdk's system/audio/GPU APIs. Keep it as a research branch/long-term option, not the shortest route to validating the existing D3D9/XAudio2 target.

## 5. CI strategy without an Xbox 360 compiler

The migration notes say a GitHub Actions Linux headless/no-op smoke workflow has already been configured, but no successful run was observed in the inspected notes. That job should be treated as pending until its run is inspected.

### Tier 1 — every pull request / push (public hosted CI)

Run on Linux and Windows using ordinary modern compilers:

- Configure and build the modern core with the existing `BACKEND=noop` / `ENABLE_NOOP_RENDERER=ON` headless target.
- Build Debug and Release where practical.
- Run available portable tests and parser tests using legally distributable test fixtures.
- Run formatting, whitespace, static analysis and compiler warnings.
- Run a source audit that checks the Xbox-specific source list and expected XDK library names without attempting to link them.
- Validate the Visual Studio project XML parses and that required source files listed in the project exist.
- Optionally run a Windows desktop compile of the portable core with modern MSVC, but label it **portable-core validation**, not Xbox compiler validation.

This tier catches ordinary C/C++ mistakes and regressions in portable code. It cannot validate Xbox headers, XDK libraries, Xenon ABI, Xbox linker behavior or XEX output.

### Tier 2 — opt-in XDK build (only if a licensed environment exists)

Use a **self-hosted, isolated Windows runner** with a locally installed and licensed XDK. Do not upload the XDK, its headers, libraries, symbols or compiler to GitHub Actions artifacts/cache. Keep credentials, XBDM endpoints and proprietary paths out of logs.

The job should record:

- XDK version/build and install path (not SDK contents).
- Visual Studio/MSBuild and compiler versions.
- Configuration/platform (`Debug|Xbox 360` or `Release|Xbox 360`).
- Exact source commit.
- Build result and sanitized diagnostics.

The XEX may be retained as a private/local artifact only if the SDK license permits it. If licensing or distribution terms are unclear, do not upload it.

### Tier 3 — emulator/hardware smoke test

This is manual or performed on a controlled lab machine/console. CI should publish a build artifact only when the build environment's license permits it, then record a separate test report with the emulator version or console model/kernel, test title, steps, logs and result.

Do not call a Linux no-op build an XDK build. Do not call an XEX that merely links a working game. Keep build, emulator and physical-console results as separate statuses.

## 6. Validation before an XDK build is available

A useful validation ladder is:

1. **Portable core compile:** compile the current modern C core on GCC/Clang and modern MSVC where supported.
2. **Headless smoke:** use the no-op renderer and no-op/disabled platform pieces to exercise startup, data parsing and VM paths that do not require graphics/audio hardware.
3. **Static analysis:** run clang-tidy or equivalent on portable sources; use compiler warnings and sanitizers (ASan/UBSan) on desktop builds where supported.
4. **Interface checks:** compile small translation units that include the renderer/audio/filesystem headers and verify every vtable member is initialized. Add compile-time assertions for structure sizes/offsets only where the platform contract is known.
5. **Adapter contract tests:** mock the platform boundary and test file read/write byte counts, directory behavior, audio lifecycle state transitions and renderer resource ownership without pretending to test the XDK APIs.
6. **XDK project consistency:** parse the `.vcxproj`, verify all referenced source/header paths, ensure Debug/Release libraries are distinct and check that feature defines match the intended build.
7. **XDK API probe:** once any XDK-compatible compiler is available, build a tiny executable that includes the same headers and links one function from each required XDK library before compiling the full runner.
8. **Real target build:** only then attempt the full XEX. Record compiler/linker output and preserve the first reproducible error.

For this project specifically, completing the modern D3D9 vtable is a prerequisite to meaningful runtime startup. A successful compile of the filesystem or audio adapter alone will not remove the renderer blocker.

## 7. Emulator and hardware testing

### Xenia

[Xenia](https://xenia.jp/) and [Xenia Canary](https://github.com/xenia-canary/xenia-canary) are Xbox 360 research emulators. Canary is the more experimental fork and is frequently updated. They are useful for quick XEX smoke tests, crash detection, basic input/audio/render checks and repeatable debugging.

Limits:

- Xenia is experimental and does not perfectly reproduce every hardware/kernel behavior.
- A title booting in Xenia does not establish correctness on a real console.
- XBDM remote deployment/debugging workflows may not be emulated exactly; use local XEX loading where supported.
- Keep emulator version, configuration, logs and test content recorded so results are reproducible.

### Real hardware

- A genuine Xbox 360 development kit is the natural target for XDK deployment/debugging.
- A JTAG/RGH console can run unsigned homebrew and is useful for XEX runtime validation. OXDK documents XBDM deployment to a console using its `xbdm.py` utility.
- Retail stock consoles generally cannot run arbitrary unsigned homebrew XEX files without an authorized development environment or a legitimate homebrew-enabling modification.
- Test on hardware for GPU behavior, audio timing, memory pressure, filesystem edge cases, controller behavior and performance.

Use Xenia for fast iteration and a real console/devkit for final confidence. Neither should be described as having passed until the exact build is run and its result is recorded.

Sources: [Xenia official site](https://xenia.jp/), [Xenia Canary](https://github.com/xenia-canary/xenia-canary), [OXDK Xbox 360 guide](https://github.com/MrMilenko/OXDK/blob/main/docs/xbox360.md).

## 8. Legitimate access to development tools

The Xbox 360 XDK was a proprietary developer kit distributed to registered/licensed developers under program terms and confidentiality obligations. Public documentation about the historical program says the program has since been discontinued; this research did not find a current public Microsoft download path for the legacy Xbox 360 XDK.

Legitimate routes to investigate:

- If the project owner or a contributor already has a valid historical developer agreement and XDK entitlement, use the SDK only within that agreement's permitted scope and keep it private.
- Ask Microsoft/Xbox developer support whether any legacy entitlement or archival access remains available to an existing licensed partner. Do not assume enrollment in today's ID@Xbox/GDK program grants Xbox 360 XDK access.
- Use OXDK only with an XDK that the user is legally entitled to possess and use; OXDK explicitly supplies no Microsoft SDK code.
- If no legitimate XDK is available, continue with portable CI and evaluate open-source alternatives such as xsdk or libxenon as separate backend projects.

Do not download or redistribute leaked XDK installers, headers, import libraries, symbols, firmware or confidential documentation. The existence of a copy on an archive or forum does not establish a license to use it.

Sources: [Unofficial Xbox 360 Development Guide — environment and SDK history](https://xb360.dev/development-environment/), [Microsoft GDK announcement — modern GDK/GDKX does not support Xbox 360](https://developer.microsoft.com/en-us/games/articles/2021/07/meet-the-microsoft-game-developer-kit-gdk/), [Microsoft ID@Xbox onboarding](https://learn.microsoft.com/en-us/gaming/game-publishing/publishing-processes/managed-creators/publishing-processes-onboarding-new-creator).

## 9. Step-by-step setup plan

### Path A — reproduce the native XDK build

1. Create a clean Windows VM and snapshot it before installing legacy tools.
2. Obtain the XDK through a valid entitlement. Record its exact version and confirm that the installation includes the Xbox 360 platform toolset, libraries, compiler and deployment/debug tools.
3. Install the Visual Studio/MSBuild generation required by that XDK. For this project, investigate VS2010/MSBuild 4.0 first because the checked-in project is ToolsVersion 4.0. If the XDK's own integration explicitly requires VS2005/2008, use a separate matching VM rather than forcing incompatible versions together.
4. Install the XDK and confirm the Xbox 360 platform is visible in Visual Studio.
5. Clone the repository and check out `refresh2/modern-upstream-migration`. Confirm the checkout SHA matches the reviewed branch tip or record the new tip.
6. Open `Butterscotch360.vcxproj`; select `Debug | Xbox 360`. Do not retarget or alter the project settings before the first baseline attempt.
7. Build. Save the complete compiler/linker log. Classify errors as missing platform integration, missing XDK header/library, source-language/compiler incompatibility, or actual source error.
8. Repeat for `Release | Xbox 360`. Verify that the expected debug/release XDK library variants are selected.
9. Once a XEX is produced, first test it in Xenia Canary, then on an RGH/JTAG console or XDK. Record separate results.

### Path B — OXDK feasibility spike

1. On Linux (or macOS), clone [OXDK](https://github.com/MrMilenko/OXDK) and follow its documented setup.
2. Build the patched LLVM/Clang toolchain with `./oxdk build-llvm`.
3. Point `XDK_DIR` at a legally obtained extracted XDK containing `lib/xbox/xboxkrnl.lib`; run `./oxdk doctor`.
4. Build and run the OXDK `abitest`, then its D3D9 clear-screen sample. Do not skip the ABI test.
5. Create a separate minimal test that includes the project's XDK headers and links D3D9, XAudio2 and the other libraries used by the project. Check the exact XAudio2 library and import requirements.
6. Compile a small subset of the project adapters. Resolve C++ runtime, name mangling, exceptions, threads, floating-point printf and calling-convention issues before attempting the full source list.
7. Compare the generated XEX and runtime behavior with the native XDK build if one becomes available. Treat OXDK as experimental until the full project is built and tested.

### Path C — no XDK available

1. Keep the existing no-op/headless GitHub Actions workflow as the required baseline and verify that it actually passes.
2. Add portable parser/VM tests and adapter contract tests with mocks.
3. Run static analysis and sanitizers on desktop builds.
4. Validate project-file consistency and compile-time interface completeness.
5. Track D3D9 renderer completion independently; the current migration audit records it as a runtime startup blocker.
6. Evaluate xsdk only as a separate backend proposal with explicit graphics/audio/filesystem port estimates. Do not replace the existing XDK backend just to get a green CI badge.

## 10. Sources and confidence notes

### Primary/technical references

- [Microsoft: Meet the GDK](https://developer.microsoft.com/en-us/games/articles/2021/07/meet-the-microsoft-game-developer-kit-gdk/) — Microsoft statement that GDK/GDKX does not support Xbox 360.
- [Microsoft Learn: MSBuild Toolset (ToolsVersion)](https://learn.microsoft.com/en-us/visualstudio/msbuild/msbuild-toolset-toolsversion) — meaning of ToolsVersion 4.0 and VS2010/2012 project generation.
- [Microsoft Learn: C/C++ language conformance](https://learn.microsoft.com/en-us/cpp/visual-cpp-language-conformance) — compiler feature availability by Visual Studio version.
- [Microsoft Learn: `/std` compiler option](https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version) — C language mode and C11/C17 support.
- [CloudBees: Xbox 360 XDK versions and Visual Studio plug-ins](https://docs.cloudbees.com/docs/cloudbees-build-acceleration-kb/latest/cloudbees-build-acceleration-kb/kbea-00013-xbox-360-development-kit-versions) — historical XDK release examples and VS2005/2008 plugin compatibility note.
- [OXDK repository](https://github.com/MrMilenko/OXDK) and [Xbox 360-specific documentation](https://github.com/MrMilenko/OXDK/blob/main/docs/xbox360.md) — current Clang/XDK bridge, ABI caveats and build/run steps.
- [libxenon repository](https://github.com/X360Tools/libxenon) — independent bare-metal Xbox 360 toolchain.
- [xsdk repository](https://github.com/xenosworks/xsdk) — newer open-source Xbox 360 SDK, system/audio/GPU scope and build instructions.
- [Xenia](https://xenia.jp/) and [Xenia Canary](https://github.com/xenia-canary/xenia-canary) — emulator project and experimental fork.
- [Unofficial Xbox 360 Development Guide](https://xb360.dev/development-environment/) — historical environment, licensing and testing overview. This is community documentation, not a Microsoft policy page.

### Confidence labels

- **Verified from inspected project files:** the current project configuration, linker library names, ToolsVersion, compile-as-C++ setting, branch tip and recorded upstream/original baseline SHAs.
- **Verified from cited documentation:** GDK does not support Xbox 360; OXDK requires a user-supplied XDK and documents its Clang/Xenon ABI process and C++ limitations; libxenon is a bare-metal alternative; modern MSVC C11/C17 support is much newer than the legacy compiler era.
- **Third-party historical evidence:** specific older XDK release numbers and Visual Studio add-in compatibility; community guides recommending VS2010 and particular XDK releases.
- **Engineering assessment / not yet tested here:** whether this exact project can build with a given XDK release, whether OXDK can link its complete XAudio2/D3D9 adapter set, and whether xsdk/libxenon can host a future backend.
- **Not performed:** no XDK installation, Xbox compilation, XEX generation, Xenia launch or physical-console test was performed for this report.

---

**Bottom line:** first reproduce the existing VS2010-era XDK project in an isolated, properly licensed environment. In parallel, OXDK is the best experiment for building against real XDK libraries from a modern host. Keep hosted CI focused on portable core correctness, and treat XEX/emulator/hardware validation as separate gates.

---

# SECOND RESEARCH PASS

**Second-pass date:** 2026-11-13  
**Scope:** revalidation against the current working branch, source-level audit of the XDK adapters and build project, and concrete implementation/test recommendations. This pass does not claim an Xbox build or runtime test.

## 11. Revalidation and current repository state

The branch `refresh2/modern-upstream-migration` still exists. A GitHub compare from the prior report commit `5ac4091448877fc1ce738f69d77b0c64df956925` to the branch reports **15 commits ahead, 0 behind**. The compare lists work from other contributors in:

- `docs/research/AUDIO_FILESYSTEM_AUDIT.md`
- `docs/research/GAME_COMPATIBILITY.md`
- `docs/research/INDEPENDENT_CODE_REVIEW.md`
- `src/common.h`
- `src/xbox360-xdk/main.cpp`
- `src/xbox360-xdk/xaudio2_audio.cpp`
- `src/xbox360-xdk/xdk_file_system.cpp`

The existing XDK report was fetched from the branch before this update and its content was preserved in full. The branch comparison did not list `docs/research/XDK_BUILD_ENVIRONMENT.md` among those 15 commits, so the contributor work above was not overwritten or folded into this file.

The current upstream `ButterscotchRunner/Butterscotch` `main` ref compares identical to the previously inspected upstream baseline `0c94bfb05e3cb4b8946a38f8085747035dd37f94` at the time of this pass. This means the old upstream SHA remains a valid baseline for this review; it is not evidence that the repository will never change again.

### Current source snapshots inspected

The following Git blob IDs identify the exact files reviewed from `refresh2/modern-upstream-migration`:

| File | Blob SHA |
|---|---|
| `Butterscotch360.vcxproj` | `2fc2bb5bc081339d02d0156f8829310d0fdfed7d` |
| `src/renderer.h` | `ae09f7dc910d0f072d89baf569a6826607d56054` |
| `src/common.h` | `eaf4b51148de24be014951c36eee4156a109fc5d` |
| `src/xbox360-xdk/d3d9_renderer.cpp` | `aed24e9a03f376046822b851174e6879434c4f48` |
| `src/xbox360-xdk/d3d9_renderer.h` | `b1f365a341d64b393b8d2be1ae31d4f8adb9df38` |
| `src/xbox360-xdk/xaudio2_audio.cpp` | `84bb2863b60b37507f9898c1597115f1ac13da09` |
| `src/xbox360-xdk/xdk_file_system.cpp` | `84bb2863b60b37507f9898c1597115f1ac13da09` |
| `src/xbox360-xdk/main.cpp` | `3c8e1cb4d7dc864d4a40c1bae9db3aab51d8378a` |

**Correction to the original report's framing:** the D3D9 problem is more severe and more specific than simply “the legacy renderer does not satisfy the modern vtable.” The current implementation uses a positional initializer whose entries are shifted against the modern vtable layout. This is a concrete ABI/API wiring defect, not just a list of missing optional features.

## 12. Critical renderer finding: positional vtable initializer is incompatible

### Confirmed source facts

The current `src/renderer.h` declares a modern `RendererVtable` beginning with this order:

1. `init`
2. `destroy`
3. `beginFrame`
4. `endFrameInit`
5. `endFrameEnd`
6. `beginView`
7. `endView`
8. `applyProjection`
9. `beginGUI`
10. `setGuiProjection`
11. `endGUI`
12. `drawSprite`
13. `drawSpritePart`
14. `drawSpritePartColor`
15. `drawSpritePos`
16. `drawRectangle`
17. ...many more entries, through surfaces, shaders, texture queries and depth state.

But `src/xbox360-xdk/d3d9_renderer.cpp` still initializes `d3d9RendererVtable` positionally with only 16 values:

```cpp
static RendererVtable d3d9RendererVtable = {
    d3d9Init,
    d3d9Destroy,
    d3d9BeginFrame,
    d3d9EndFrame,
    d3d9BeginView,
    d3d9EndView,
    d3d9DrawSprite,
    d3d9DrawSpritePart,
    d3d9DrawRectangle,
    d3d9DrawLine,
    d3d9DrawLineColor,
    d3d9DrawText,
    d3d9Flush,
    d3d9CreateSpriteFromSurface,
    d3d9DeleteSprite,
    NULL,
};
```

That initializer is not merely short. From entry 4 onward it assigns functions to the wrong callback slots. For example:

| Modern slot | Value currently assigned | Actual intended function |
|---|---|---|
| `endFrameInit` | `d3d9EndFrame` | needs a modern end-frame-init implementation |
| `endFrameEnd` | `d3d9BeginView` | needs the end-of-frame callback |
| `beginView` | `d3d9EndView` | needs begin-view |
| `endView` | `d3d9DrawSprite` | needs end-view |
| `applyProjection` | `d3d9DrawSpritePart` | needs projection application |
| `beginGUI` | `d3d9DrawRectangle` | needs GUI target/viewport setup |
| `setGuiProjection` | `d3d9DrawLine` | needs GUI projection |
| `endGUI` | `d3d9DrawLineColor` | needs GUI pass teardown |
| `drawSprite` | `d3d9DrawText` | needs sprite draw |
| `drawSpritePart` | `d3d9Flush` | needs sprite-part draw |
| `drawSpritePartColor` | `d3d9CreateSpriteFromSurface` | incompatible callback signature |
| `drawSpritePos` | `d3d9DeleteSprite` | incompatible callback signature |
| `drawRectangle` | `NULL` | needs rectangle draw |

The remaining modern slots are zero-initialized, but several early slots already contain incorrect non-null function pointers. A runtime “non-null vtable” check is therefore insufficient: a callback can be non-null and still have the wrong signature/meaning.

### Required implementation change

Replace the positional initializer with designated/member assignments in a one-time initializer, following the pattern already used by the audio and filesystem adapters:

- `memset(&d3d9RendererVtable, 0, sizeof(...))`
- Assign every callback by member name.
- Keep genuinely unsupported optional callbacks null only where the modern runner explicitly supports null.
- Implement required callbacks before setting the renderer as usable.
- Add a compile-time or unit-test audit that enumerates required renderer members and fails if any required entry is null.
- Do not rely on compiler warnings to detect this: the old project uses warning level 3 and does not declare warnings-as-errors.

For an initial bring-up, implement the minimum valid frame/view/GUI/sprite/primitive callbacks first, but use named assignments so missing entries remain visible. Do not map unrelated legacy functions into “closest-looking” slots.

### Renderer tests

1. **Vtable wiring test:** verify every required callback points to the intended named implementation; specifically assert that `endFrameInit`, `endFrameEnd`, `beginView`, `endView`, `applyProjection`, `beginGUI`, `setGuiProjection`, `endGUI`, `drawSprite` and `drawSpritePart` are not shifted.
2. **Signature test:** compile each callback assignment with warnings enabled and treat incompatible function-pointer diagnostics as errors in the portable test target.
3. **Lifecycle test with a mock device:** verify begin-frame, begin-view, end-view, GUI and end-frame callbacks occur in the same order as `Runner_drawPre`, `Runner_beginFrame`, `Runner_drawViews`, `Runner_drawPost` and `Runner_drawGUI`.
4. **First-room smoke test:** ensure the renderer survives `Runner_create` and `Runner_initFirstRoom` without calling a null or miswired callback.
5. **Visual smoke test:** render one textured quad, one untextured rectangle, one line, one glyph and one alpha-blended sprite before attempting a full game.

## 13. Additional D3D9/XDK risks found in the current implementation

These are source-review findings or validation gaps, not claims of observed hardware failures.

### Resource creation and failure propagation

In `d3d9Init`, the renderer allocates `vertexData`, compiles two shaders, creates shader objects and a vertex declaration, creates the white texture, allocates texture arrays, then uploads every TXTR page. Several operations check only some failures:

- `malloc`/ `calloc` results are not consistently checked before later use.
- `CreateVertexShader`, `CreatePixelShader`, `CreateVertexDeclaration`, `CreateTexture`, `LockRect` and `UnlockRect` results are not consistently propagated.
- Shader compilation failure returns from a `void` init callback after logging, but there is no explicit renderer-ready state consumed by `main.cpp` or `Runner_create`.
- The TXTR upload path stores a null texture after decode or texture-creation failure and continues. That can become a later draw-time fallback or missing texture rather than a clear startup error.

**Recommendation:** add an explicit `initialized/failed` state to `D3D9Renderer`; check every HRESULT and allocation; release partially created resources through one idempotent cleanup path; and make startup abort with a clear diagnostic if renderer initialization fails. Since the current generic `RendererVtable.init` returns `void`, do not silently pretend it can report success. Either validate the backend before passing it to `Runner_create`, or propose a coordinated core API change with an explicit status return. Avoid changing only the XDK adapter to a new function signature that no longer matches the shared contract.

### Texture upload path needs hardware validation

The current code decodes PNG data with `stbi_load_from_memory`, writes pixels to `D3DFMT_LIN_A8R8G8B8`, and comments that linear textures can be sampled directly on Xbox 360. The code does not create the tiled copy described in its own comment. This may be intentional if the selected XDK path supports the chosen linear texture usage, but it must not be assumed from the comment.

Test at least:
- 1x1, odd-width, non-power-of-two and maximum expected TXTR dimensions.
- Alpha edges and fully transparent pixels.
- A texture with distinct red/green/blue/alpha channel values to expose endian/channel-order mistakes.
- A texture large enough to cross relevant pitch/alignment boundaries.
- Sampling the same texture through sprite and font paths.

Check all `CreateTexture`, `LockRect`, `UnlockRect` and device draw HRESULTs, and confirm the required pitch/format/alignment rules against the exact XDK documentation for the chosen XDK release.

### Draw batching and primitive assumptions

`flushBatch` uses `DrawPrimitiveUP(D3DPT_QUADLIST, ...)`, while the generic renderer contract exposes points, lines, line strips, triangles, triangle strips and triangle fans. The legacy quad batching path does not by itself implement the modern generic primitive/vertex-buffer contract.

Verify that `D3DPT_QUADLIST` is supported by the exact Xbox 360 D3D9 API/toolchain being targeted; do not infer support from desktop D3D9. If the Xbox-specific API does not support it, convert each quad into two triangles and use a supported primitive type. Add tests for winding order, UV orientation, alpha blending and batch flushes at 1, 2048 and 2049 quads.

## 14. Audio adapter: contract coverage is better, but semantics remain incomplete

### Confirmed improvements

The current `xaudio2_audio.cpp` initializes the modern audio vtable by named member assignments, avoiding the positional-shift problem found in D3D9. It binds the 31 modern entries and explicitly provides stubs for unsupported streaming and encoded playback rather than leaving arbitrary function pointers.

The current implementation handles decoder-backed WAV/OGG playback, XAudio2 voices, per-instance gain/pitch, looping, pause/resume, master gain, audio groups and lazy AUDO loading. It also defines explicit little-endian WAV readers and disables stb_vorbis's fast scaled-float path for the big-endian Xenon target.

### Remaining behavior and lifecycle questions

- `xdkCreateStream`, `xdkDestroyStream` and `xdkPlayEncoded` intentionally return failure/no-op results. Confirm the runner handles these failures gracefully for every game path that requests them.
- `xdkSetSoundSpatial` and listener-position handling are stubs; spatial audio will not be equivalent to a backend that implements positional routing.
- `xdkAudioInit` can return early after XAudio2 or mastering-voice creation fails. The generic init callback is `void`; startup needs a way to detect that the audio backend is unavailable.
- The current backend decodes OGG/WAV into PCM and creates source voices. Measure peak memory with many simultaneous sounds and large embedded AUDO assets; the Xbox 360 memory budget makes full-file decode a meaningful constraint.
- Confirm pitch semantics for SOND pitch versus runtime pitch changes, and verify that the XAudio2 frequency-ratio range supports the runner's accepted pitch range.
- Confirm every voice is stopped/destroyed before its PCM buffer is freed, including all early-error paths and shutdown while sounds are active.

### Audio tests

1. Play embedded WAV and OGG assets; test external OGG file loading separately.
2. Test zero-length, truncated, malformed and oversized WAV/OGG/AUDO data.
3. Test mono/stereo and sample-rate variations supported by the game corpus.
4. Exercise simultaneous voices up to and beyond `XDK_MAX_SOUND_INSTANCES`; verify the documented priority policy rather than silently dropping important sounds.
5. Test looping, pause/resume, stop, fade-to-gain, master gain and audio-group gain independently and in combination.
6. Test lazy group load, repeated group load, group failure and shutdown while a group is loaded.
7. Call stream/encoded APIs from a mock runner and confirm the runner handles their explicit failure values without dereferencing invalid stream state.
8. Run long-duration playback and verify voice reclamation, PCM ownership and memory stability.

## 15. Filesystem adapter: implementation details and edge cases

### Confirmed implementation

The current `xdk_file_system.cpp` uses named assignments for all 20 modern filesystem callbacks. It implements text/binary access, rename/delete, streaming handles, directory operations and enumeration. It loops on partial `ReadFile`/`WriteFile` results for whole-file helpers, checks byte counts and rejects files beyond the signed 32-bit engine API limit. It also handles the Win32 `SetFilePointer` sentinel correctly by checking `GetLastError`.

### Additional concerns

- `xdkResolvePath` concatenates the base directory with the supplied path and converts forward slashes. It treats drive-qualified and root-leading paths as absolute, but does not normalize or reject `..` traversal segments. If the intended contract is “relative to the game data directory,” add normalization and explicit escape tests. If absolute paths are intentionally part of GameMaker compatibility, document that policy and test it rather than accidentally restricting valid game behavior.
- The binary streaming API uses 32-bit positions/sizes. This is consistent with the current engine API's signed 32-bit limits, but files above that range must fail predictably rather than wrap.
- `xdkListDirectory` silently skips an entry if allocating its name fails. This is a defensible best-effort policy only if the caller can tolerate incomplete enumeration; otherwise surface an error or define the behavior in the contract.
- Test drive roots, `game:\\`, `d:\\`, nested paths, empty relative paths, trailing separators, mixed slash styles, missing files, read-only files, rename across directories, and directories containing both files and subdirectories.
- Confirm the lifetime/ownership contract for the dynamic-array result of `listDirectory`: callers must free each entry name and the array using the same allocator conventions. A mismatch here can leak or corrupt memory on shutdown.

### Filesystem tests

1. Whole-file read/write with empty, one-byte, multi-megabyte and boundary-size files.
2. Streaming open/read/tell/seek/rewrite/write/close, including seeks to start/end and invalid negative offsets.
3. Partial-read/write simulation where feasible, and explicit zero-byte progress/error behavior.
4. Rename/delete behavior for existing, missing, read-only and cross-directory files.
5. Directory create/list/delete with nested directories and mixed file types.
6. Path resolution for `foo/bar`, `foo\\bar`, absolute drive paths, root-leading paths and `..` segments.
7. Verify no 32-bit size/offset truncation and no leaked handles after every failure path.

## 16. Startup and project-file findings

### XDK project file

The current `Butterscotch360.vcxproj` still declares `ToolsVersion="4.0"`, `Debug|Xbox 360` and `Release|Xbox 360`, forces `compat_msvc.h`, compiles all listed `.c` files as C++, disables exceptions/RTTI and links the XDK-specific library variants. It does **not** declare a `PlatformToolset` value in the project file. Therefore `ToolsVersion=4.0` alone does not identify the exact Xbox compiler, XDK release or installed platform targets; the matching XDK integration must supply the Xbox 360 platform props/targets and toolchain.

The project also has an empty `<ImageXex>` element and uses XDK deployment metadata with `RemoteRoot=xe:\\Butterscotch`. Confirm the XDK's imported targets supply the expected linker/XEX packaging defaults. Do not assume a successful MSBuild evaluation means a deployable XEX has been produced.

### Main startup checks

The current `main.cpp` creates the D3D device, parses `data.win`, creates filesystem/renderer/VM/audio objects, then calls `Runner_create` and `Runner_initFirstRoom`. Some subsystem creation results are not checked before use. In particular, add explicit checks for:

- `XdkFileSystem_create`
- `D3D9Renderer_create`
- `VM_create`
- `XdkAudioSystem_create`
- `Runner_create`

The startup path also uses an infinite loop when `data.win` is missing or parsing fails. That may help a debugger remain attached, but it is not a useful production failure mode. Prefer a clear diagnostic plus controlled cleanup/exit, with an optional debug-only hang switch.

The modern runner owns subsystem initialization through `Runner_create`; the entry point must not separately initialize the same renderer/audio backend. Add a single documented ownership table stating which function allocates, initializes and destroys each object. The existing cleanup sequence manually destroys audio and renderer, nulls `runner->audioSystem`, then frees the runner and VM/data. Verify this against `Runner_free` so no resource is destroyed twice and no object is freed while still referenced.

### Project/build validation tasks

- Parse the project XML in CI and validate all explicit `ClCompile` paths/globs resolve to tracked files.
- Check the Debug and Release library lists independently.
- Check that the Xbox platform props/targets and compiler are present before invoking MSBuild; fail with a targeted message if they are absent.
- Capture the evaluated MSBuild properties and exact compiler/linker command lines in a private local log, redacting proprietary installation paths where necessary.
- Verify the output is a valid XEX with the expected entry point, imports and packaging metadata; a successful compile-only target is not enough.

## 17. Prioritized implementation plan

1. **P0 — fix renderer vtable wiring.** Replace the positional initializer with named member assignments. Add a test that checks required callbacks and catches incompatible signatures.
2. **P0 — establish subsystem initialization failure handling.** Check all object constructors and add a supported way to detect renderer/audio initialization failure before first-room startup.
3. **P1 — complete minimum modern renderer lifecycle.** Implement frame/view/projection/GUI hooks with correct render-target and viewport semantics before expanding drawing features.
4. **P1 — add renderer resource/error handling.** Check allocations and HRESULTs; clean up partially initialized devices/textures/shaders safely.
5. **P1 — validate filesystem path and ownership semantics.** Add path normalization policy, 32-bit boundary tests, enumeration ownership tests and handle-leak tests.
6. **P1 — test audio failure behavior and memory pressure.** Especially large AUDO decode, many concurrent voices, voice cleanup and unsupported stream/encoded paths.
7. **P2 — validate XDK project integration.** Confirm the actual XDK platform props/targets, compiler, linker, XEX packaging and deployment commands on a legitimately licensed environment.
8. **P2 — run emulator and hardware gates separately.** Keep portable CI, XDK compile, Xenia boot and real-console results as separate status records.

## 18. Second-pass confidence and limitations

- **Confirmed from current source:** the D3D9 vtable is initialized positionally against a substantially expanded modern struct, causing callback misalignment; audio and filesystem vtables use named assignments; the XDK project uses ToolsVersion 4.0 but does not declare a PlatformToolset; the startup path lacks several constructor-result checks.
- **Confirmed from GitHub compare:** the working branch contains 15 commits beyond the prior report commit and the listed contributor changes do not include this XDK report; modern upstream `main` is identical to the previously recorded upstream baseline during this pass.
- **Engineering risks requiring target validation:** Xbox-specific `D3DPT_QUADLIST` behavior, linear texture sampling/pitch/format rules, exact XAudio2 API/library compatibility, actual XEX packaging defaults, XDK compiler behavior and filesystem path semantics on target storage.
- **Not performed:** no Xbox 360 XDK compiler was available; no XDK build, XEX generation, Xenia execution or physical-console test was performed. No claim of runtime success is made.
