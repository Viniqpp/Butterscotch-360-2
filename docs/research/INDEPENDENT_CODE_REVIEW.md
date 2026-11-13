# Independent Code Review — Butterscotch 360 Refresh 2

**Review date:** 2026-11-13  
**Target:** `Viniqpp/Butterscotch-360-2`, branch `refresh2/modern-upstream-migration`  
**Review type:** static source review; no compilation or runtime testing performed.

## 1. Exact source baselines

| Repository | Commit inspected | Notes |
|---|---|---|
| [Viniqpp/Butterscotch-360-2](https://github.com/Viniqpp/Butterscotch-360-2) | `5ac4091448877fc1ce738f69d77b0c64df956925` | Branch tip at review time. Its only changed file relative to the preceding source snapshot is `docs/research/XDK_BUILD_ENVIRONMENT.md`; platform/core source was reviewed at the preceding tip `39fc2d4b9c5163f7d1a931a743e8767991974a6c`. |
| [ButterscotchRunner/Butterscotch](https://github.com/ButterscotchRunner/Butterscotch) | `0c94bfb05e3cb4b8946a38f8085747035dd37f94` | Modern upstream `main` commit inspected. |
| [ceilingtilefan/Butterscotch-360](https://github.com/ceilingtilefan/Butterscotch-360) | `1fd7659a4b95e0ce9837c3162de976cfc9ceec5a` | Original Xbox 360 port baseline. |

The branch was verified to exist before review and again before publication. The requested report path did not exist at publication time.

## 2. Executive summary

**The current XDK executable cannot successfully construct the modern Runner with the current D3D9 renderer.** This is a confirmed source-level blocker, not merely a missing rendering feature: the renderer still initializes the old positional vtable, while modern `Runner_create()` validates required modern callbacks. The vtable's 16th entry is null and lands in modern `drawRectangle`, which validation requires. The validator aborts before normal game startup.

Other high-value review points:

1. **Confirmed blocker — renderer vtable ABI/API mismatch.** Old positional initializer is incompatible with modern `RendererVtable`; required entries are missing and several non-null pointers occupy slots with different signatures.
2. **High-risk portability issue — big-endian detection.** The Xbox project defines `_XBOX`, but the imported byte-order detection does not test that macro. If the historical XDK compiler does not also define one of the GCC-style endian macros, little-endian data.win fields will be read without byte swapping on the big-endian Xenon CPU.
3. **Confirmed error-path undefined behavior — CONFIG.JSN read.** The code ignores `ReadFile`'s result and may index using an uninitialized `bytesRead` value.
4. **Confirmed robustness gaps — unchecked subsystem allocations and D3D resource creation.** Several failure returns are not propagated, allowing later code to use partially initialized objects/resources.
5. **XDK ABI, calling convention, alignment and runtime behavior remain unverified.** No XDK compiler, SDK headers/libraries, console or emulator build was available in this review.

The audio adapter populates all 31 modern audio vtable entries, and the filesystem adapter populates all 20 modern filesystem entries. Their implementation details still need platform testing, but I did not find a comparably obvious positional-vtable mismatch in either adapter.

## 3. Prioritized findings

### P0 — Modern Runner startup is blocked by the legacy renderer vtable

**Status:** Confirmed from source.

**Locations**
- [`src/xbox360-xdk/d3d9_renderer.cpp`, lines 721–739](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/xbox360-xdk/d3d9_renderer.cpp#L721-L739) — 16-entry positional initializer and constructor.
- [`src/renderer.h`](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/renderer.h) — modern `RendererVtable` declaration (81 entries).
- [`src/runner.c`, lines 2471–2527](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/runner.c#L2471-L2527) — `validateRendererVtable()`.
- [`src/runner.c`, lines 2558–2565](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/runner.c#L2558-L2565) — `Runner_create()` calls the validator.
- [`src/xbox360-xdk/main.cpp`, line 218](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/xbox360-xdk/main.cpp#L218) — XDK entry point calls `Runner_create()`.

The D3D9 initializer lists only the old renderer callbacks:
`init, destroy, beginFrame, endFrame, beginView, endView, drawSprite, drawSpritePart, drawRectangle, drawLine, drawLineColor, drawText, flush, createSpriteFromSurface, deleteSprite, NULL`.

The modern structure inserts new callbacks before and between those old operations. Aggregate initialization is positional, not name-based. Therefore the old fifth entry (`d3d9BeginView`) is assigned to modern `endFrameEnd`; subsequent functions are shifted into unrelated slots. More decisively, the old 16th entry is `NULL`, which initializes modern `drawRectangle`; `validateRendererVtable()` explicitly requires `drawRectangle != nullptr`. The modern validator therefore aborts during `Runner_create()`, before the main loop.

**Impact:** startup fails before the first room. If validation were bypassed, the shifted function pointers would also cause incorrect calls and potential ABI-level crashes because the expected signatures differ.

**Fix**
- Port D3D9 to the modern renderer contract deliberately. Implement every callback required by `validateRendererVtable()`; provide explicit, semantically safe implementations for unsupported features rather than relying on positional compatibility.
- Replace the positional aggregate with zero-initialization followed by named member assignments. This is compatible with older MSVC and prevents future field insertion from silently shifting callbacks.
- Add a compile-time or unit-level contract test that verifies every callback required by `validateRendererVtable()` is non-null. Keep optional callbacks explicitly documented.
- Do not weaken/remove the modern validator just to make the legacy renderer pass.

**Tests**
1. Construct the D3D9 renderer and assert all validator-required members are non-null.
2. Call `Runner_create()` with a test DataWin/VM and renderer; verify it returns normally.
3. Exercise each required callback through a mock device or test harness and verify the expected signature/arguments.
4. Add a regression test that fails if the modern vtable gains a required member without a corresponding backend assignment.

### P1 — Big-endian Xenon byte-order detection may be missing

**Status:** High-risk conditional defect; confirm compiler predefined macros with the actual XDK compiler.

**Locations**
- [`src/common.h`, lines 22–24](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/common.h#L22-L24) — defines `IS_BIG_ENDIAN` only from `__BYTE_ORDER__` / `__BIG_ENDIAN__`.
- [`src/binary_utils.h`, lines 74–96](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/binary_utils.h#L74-L96) — all little-endian conversion depends on `IS_BIG_ENDIAN`.
- [`Butterscotch360.vcxproj`](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/Butterscotch360.vcxproj) — XDK configurations define `_XBOX`.

The Xenon CPU is big-endian, while data.win serialization is little-endian. The XDK project explicitly defines `_XBOX`, but `common.h` does not use `_XBOX` or the MSVC/Xenon architecture macro to set `IS_BIG_ENDIAN`. If the XDK compiler does not also define `__BYTE_ORDER__` or `__BIG_ENDIAN__`, `BinaryUtils_toLittle16/32/64()` become no-ops and parsed numeric fields are byte-reversed.

The previous Xbox 360 port's binary reader explicitly included `defined(_XBOX)` in its byte-swap condition. That historical workaround makes this migration worth treating as a release blocker until checked, but the exact XDK compiler macro set was not available here; therefore this finding is not labelled fully confirmed.

**Impact:** corrupted chunk lengths, offsets, IDs, numeric values and floating-point data; likely parser failure or invalid runtime state.

**Fix**
- Add a narrowly scoped Xbox/Xenon big-endian condition based on a macro actually emitted by the selected XDK compiler (for example, verify `_XBOX` / `_M_PPC` in a preprocessor probe before selecting one).
- Prefer a single canonical platform-endian definition shared by all binary readers/writers.
- Avoid inferring endianness from CPU name strings or assuming GCC macros exist under MSVC.

**Tests**
1. Compile a preprocessor probe with the real XDK compiler and record all relevant architecture/endian macros.
2. On Xenon or an endian-emulating host, read fixed byte arrays representing `0x01020304`, 16-/64-bit values, IEEE-754 floats and doubles; assert exact values.
3. Parse a small known-good data.win and compare selected chunk counts/offsets/IDs against a little-endian host parse.

### P1 — CONFIG.JSN read can use an uninitialized byte count

**Status:** Confirmed error-path bug.

**Location**
- [`src/xbox360-xdk/main.cpp`, lines 187–198](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/xbox360-xdk/main.cpp#L187-L198).

`DWORD bytesRead;` is uninitialized. The return value from `ReadFile()` is ignored, and the code immediately writes `configText[bytesRead] = '\0'`. If the read fails, `bytesRead` is not guaranteed to be initialized; indexing with it is undefined behavior and can write out of bounds. The allocation result is also not checked before use. `GetFileSize()` is called without checking its error sentinel or high DWORD.

**Impact:** startup crash or memory corruption when an optional config file exists but cannot be read, or allocation fails.

**Fix**
- Initialize `bytesRead = 0`; check `ReadFile()`'s BOOL result and ensure the reported byte count does not exceed the allocation.
- Check `GetFileSize()` for `INVALID_FILE_SIZE` plus `GetLastError()`; handle/reject files larger than the parser's supported size.
- Check allocation before reading. On any failure, close the handle and ignore the optional config cleanly.
- Prefer reading exactly the reported file size and terminate at the actual number of bytes read.

**Tests**
1. Valid empty and non-empty CONFIG.JSN.
2. File disappears/is denied between open and read; simulated `ReadFile` failure.
3. Allocation failure and oversized file.
4. Truncated JSON input; verify parser failure does not corrupt startup state.

### P2 — D3D9 initialization ignores allocation and resource-creation failures

**Status:** Confirmed unchecked failures; downstream crash risk.

**Locations**
- [`src/xbox360-xdk/d3d9_renderer.cpp`, lines 123–220](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/xbox360-xdk/d3d9_renderer.cpp#L123-L220) — `d3d9Init()`.
- [`src/xbox360-xdk/d3d9_renderer.cpp`, lines 721–739](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/xbox360-xdk/d3d9_renderer.cpp#L721-L739) — `D3D9Renderer_create()`.

`d3d9Init()` does not check the vertex staging-buffer allocation. Several D3D calls (shader creation, vertex declaration creation, texture creation and lock operations) have HRESULT/results that are ignored. Shader compilation failures return from a void initializer, leaving the renderer partially initialized with no failure status propagated to Runner. Texture upload similarly continues after unchecked texture/lock failures. `D3D9Renderer_create()` dereferences the result of `calloc()` without checking it.

**Impact:** null dereference, invalid GPU resource use, incomplete rendering or leaks after partial initialization.

**Fix**
- Make initialization report success/failure through an explicit backend state or a revised initialization contract.
- Check every allocation, HRESULT and LockRect result before using returned pointers/resources.
- Unwind partially created resources on failure; make destroy safe for every partial-init state.
- Do not permit the main loop to run unless renderer initialization completed successfully.

**Tests**
1. Fault-inject each allocation and D3D resource-creation failure.
2. Verify partial initialization unwinds all resources exactly once.
3. Exercise destroy after failure at each initialization stage.
4. Run a texture upload test with padded pitch and a deliberately failed lock.

### P2 — XAudio2 subsystem allocation results are unchecked

**Status:** Confirmed unchecked failures; OOM-only risk.

**Locations**
- [`src/xbox360-xdk/xaudio2_audio.cpp`, lines 756–762](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/xbox360-xdk/xaudio2_audio.cpp#L756-L762) — `XdkAudioSystem_create()`.
- [`src/xbox360-xdk/xaudio2_audio.cpp`, lines 95–119](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/xbox360-xdk/xaudio2_audio.cpp#L95-L119) — `xdkAudioInit()`.

The constructor dereferences `xa` after `calloc()` without checking it, and stores `instanceData` without checking its allocation. Later `Instances()` callers assume this array exists. Audio initialization also returns void and leaves the subsystem present but uninitialized if XAudio2/mastering voice creation fails; callers must consistently tolerate that state.

**Impact:** allocation failure can crash during construction or first audio operation. Device initialization failure must not leave call sites assuming audio is active.

**Fix**
- Check both allocations and return a clean failure from construction.
- Make the uninitialized state an explicit supported mode, or fail Runner creation if audio is mandatory.
- Guard all code paths that dereference instance storage; preserve safe teardown after partial initialization.

**Tests**
1. Fault-inject allocation failure for the object and instance array.
2. Simulate XAudio2Create and CreateMasteringVoice failures.
3. Call update/play/stop/destroy after each failure stage; verify no null dereferences or double frees.

### P2 — Performance counter frequency is used without validation

**Status:** Confirmed missing error handling; platform failure risk.

**Locations**
- [`src/xbox360-xdk/main.cpp`, lines 28–32](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/xbox360-xdk/main.cpp#L28-L32) — `_xdk_monotonic_ms()`.
- [`src/xbox360-xdk/main.cpp`, lines 260–268](https://github.com/Viniqpp/Butterscotch-360-2/blob/refresh2/modern-upstream-migration/src/xbox360-xdk/main.cpp#L260-L268) — main-loop timer setup.

Both paths use `QueryPerformanceFrequency()` without checking success or verifying a non-zero frequency before division. The main loop then divides by `freq.QuadPart`.

**Impact:** division by zero or invalid timing if the platform call fails.

**Fix**
- Check the API return and `QuadPart > 0`; provide a documented fallback clock or abort cleanly.
- Query the frequency once and reuse the validated value.

**Tests**
- Stub QPF failure and zero frequency; verify safe fallback/error behavior.
- Verify delta-time remains monotonic and bounded under normal operation.

## 4. API/ownership review notes

### Audio vtable

`src/xbox360-xdk/xaudio2_audio.cpp` assigns all 31 modern members by name in `xdkInitializeAudioVtable()` (lines 717–751). This avoids the positional-layout defect in the renderer. The implementation intentionally returns unsupported/failure results for some advanced capabilities per the migration notes. Validate those stubs against each caller's expected sentinel and ownership contract, especially encoded playback, stream operations and spatial routing.

### Filesystem vtable

`src/xbox360-xdk/xdk_file_system.cpp` assigns all 20 modern members by name (lines 333–374). Its exact-read/write loops and handle cleanup are directionally sound. Remaining platform checks include XDK path semantics, directory enumeration behavior, file-sharing flags, 32-bit size limits and whether `SetFilePointer` behavior matches the assumptions under the target XDK.

### Frame lifecycle

`src/xbox360-xdk/main.cpp` manually sequences `Runner_step`, `Runner_drawPre`, `Runner_beginFrame`, `Runner_drawViews`, `endFrameInit`, `Runner_drawPost`, `endFrameEnd`, `Runner_drawGUI`, then `EndScene/Present`. This cannot be considered validated while the renderer vtable is shifted. After fixing the vtable, compare the exact ordering and surface/framebuffer semantics with the current upstream desktop platform loop, then test one-frame, multi-step catch-up, room-transition, paused and shutdown paths. Do not infer correctness merely from the comments in the XDK main file.

### Memory ownership and shutdown

The audio destroy routine frees its instance array, separately loaded audio-group DataWins (excluding the primary DataWin at index 0), group gains, XAudio voices and XAudio2 object. The main function then nulls `runner->audioSystem` before `Runner_free()`, which appears intended to prevent a second owner from destroying the audio subsystem. Confirm this against `Runner_free()` before changing ownership. The D3D renderer destroy routine releases its texture/shader/declaration resources and frees its backend object; ensure it is called exactly once and only after rendering has stopped.

## 5. XDK-only verification still required

The following cannot be established by this source review or by a desktop/no-op build:

- Actual XDK compiler predefined macros, especially the Xenon endian/architecture macro set.
- Xenon ABI details for all C/C++ calls, variadic functions, COM/XDK interfaces, function-pointer calling conventions and struct packing.
- Actual size/alignment/offsets of structures shared with XDK headers and hardware APIs.
- Availability and exact signatures of the targeted D3D9, XGraphics, XAudio2, XInput, kernel and CRT APIs for the chosen XDK release.
- Whether the selected XDK compiler accepts every imported source file when compiled as C++ with the current compatibility shim.
- Linker behavior, import resolution, XEX generation, stack usage and runtime library compatibility.
- Texture tiling/format behavior, D3D resource lifetime, GPU state and presentation behavior on Xenon.
- XAudio2 voice creation, timing, format support, buffer callbacks and shutdown behavior on hardware.
- Filesystem path/drive behavior on DVD, HDD, devkit and emulator.
- Real frame pacing, memory pressure, room transitions, long-session stability and application shutdown.

## 6. Recommended validation sequence

1. **Fix the renderer contract first.** Add a vtable contract test that runs the same required-member checks as `validateRendererVtable()`.
2. **Verify endianness.** Capture the real XDK compiler's predefined macros and test binary readers against known little-endian fixtures on Xenon.
3. **Add failure-injection tests** for CONFIG.JSN I/O, D3D allocations/resource creation and XAudio2 construction/init.
4. **Run portable CI** (GCC/Clang and modern MSVC where supported) with warnings, ASan/UBSan and parser/VM tests. Label this portable validation, not XDK validation.
5. **Build the XDK project** with the exact licensed XDK/Visual Studio environment; record versions, full logs and first errors.
6. **Run staged runtime tests:** boot to first room, render sprites/text, input, audio, room transitions, surfaces, repeated pause/resume, repeated startup/shutdown and long-session memory behavior.
7. **Test on Xenia and physical hardware separately.** An emulator pass is not proof of real-console correctness.

## 7. What was not done

- No source files or project configuration were modified as part of the review.
- No compilation, linking, CI workflow run, XEX generation, emulator launch or console test was performed.
- No XDK headers or binaries were available for ABI/preprocessor verification.
- Findings above are based on inspected source and are explicitly separated into confirmed defects and conditional risks.

---

**Conclusion:** The modern renderer contract mismatch is the immediate blocker. Do not spend time debugging game-level startup or frame behavior until the XDK renderer implements the required modern vtable and passes the same validation used by `Runner_create()`.