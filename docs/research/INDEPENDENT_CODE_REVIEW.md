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

---

## 8. SECOND RESEARCH PASS — current-branch revalidation and deeper XDK adapter audit

**Second-pass date:** 2026-11-13  
**Working branch:** `refresh2/modern-upstream-migration`  
**Exact fork branch HEAD inspected:** `d61018a1e326e1b0034f9dd3879677aa36523abe`  
**Upstream baseline:** `ButterscotchRunner/Butterscotch` at `0c94bfb05e3cb4b8946a38f8085747035dd37f94` (the second-pass XDK audit reported upstream `main` unchanged from this baseline during its check).  
**Method:** source and Git metadata review only. No XDK compilation, XEX generation, Xenia execution or physical-console testing was performed.

This section adds findings from the second pass and rechecks the earlier findings against the newer branch. It does not replace the original analysis. Where later commits fixed an earlier issue, that is recorded explicitly rather than leaving the old finding looking current.

### 8.1 Exact current source snapshots

The following blob IDs identify the files inspected at the branch HEAD above:

| File | Blob SHA |
|---|---|
| `Butterscotch360.vcxproj` | `2fc2bb5bc081339d02d0156f8829310d0fdfed7d` |
| `src/renderer.h` | `ae09f7dc910d0f072d89baf569a6826607d56054` |
| `src/common.h` | `eaf4b51148de24be014951c36eee4156a109fc5d` |
| `src/xbox360-xdk/d3d9_renderer.cpp` | `aed24e9a03f376046822b851174e6879434c4f48` |
| `src/xbox360-xdk/d3d9_renderer.h` | `b1f365a341d64b393b8d2be1ae31d4f8adb9df38` |
| `src/xbox360-xdk/xaudio2_audio.cpp` | `84bb2863b60b37507f9898c1597115f1ac13da09` |
| `src/xbox360-xdk/xdk_file_system.cpp` | `04d03335ca8bec185b89c6ce5d97c7c0341685c3` |
| `src/xbox360-xdk/main.cpp` | `3c8e1cb4d7dc864d4a40c1bae9db3aab51d8378a` |

### 8.2 Revalidation of first-pass findings

| Original finding | Current status at second-pass HEAD | Evidence / consequence |
|---|---|---|
| P0 renderer vtable positional mismatch | **Still present; unchanged blocker** | `d3d9_renderer.cpp` still has a 16-entry positional initializer at lines 721–739, while modern `RendererVtable` has the expanded callback layout beginning at `renderer.h` line 137. The callbacks are shifted, not simply absent. |
| P1 Xenon big-endian detection | **Fixed in current source** | `common.h` now explicitly includes `defined(_XBOX)` and `defined(_M_PPC)` in `IS_BIG_ENDIAN`. The earlier conditional concern should not be treated as an outstanding current defect for this project configuration. |
| P1 CONFIG.JSN uninitialized byte count | **Fixed in current source** | `main.cpp` lines 191–211 validates file size (including the invalid-size sentinel and a 1 MiB cap), checks allocation, initializes `bytesRead`, checks `ReadFile`, requires a complete read and closes the handle. |
| P2 unchecked XAudio2 object/instance-array allocations | **Fixed in current source** | `XdkAudioSystem_create()` lines 879–892 checks both `calloc` results and frees the parent allocation if the instance-array allocation fails. |
| P2 D3D9 allocation/resource failure propagation | **Still present; broadened below** | The D3D9 initializer continues to ignore or incompletely propagate important HRESULTs and allocation failures; its `void` init contract gives the caller no success result. |
| P2 performance-counter frequency validation | **Still present** | `main.cpp` line 30 and the main-loop timer setup still call `QueryPerformanceFrequency` without checking its return value or verifying a positive frequency before division. |

The first-pass report therefore remains correct about the renderer blocker, but its endian, CONFIG.JSN and XAudio2 constructor-allocation findings are historical defects that later branch changes have addressed. The remaining timing issue is still relevant.

### 8.3 P0 renderer finding: the defect is callback miswiring, not just missing callbacks

**Status:** Confirmed from current source; highest-priority integration blocker.

The modern `RendererVtable` begins with `init`, `destroy`, `beginFrame`, `endFrameInit`, `endFrameEnd`, `beginView`, `endView`, `applyProjection`, GUI lifecycle callbacks, and then the drawing callbacks (`renderer.h`, around lines 137–160). The XDK backend still initializes only 16 entries positionally (`d3d9_renderer.cpp`, lines 721–739).

The resulting assignments include:

| Modern member | Current positional value | Problem |
|---|---|---|
| `endFrameInit` | `d3d9EndFrame` | Wrong lifecycle operation |
| `endFrameEnd` | `d3d9BeginView` | Wrong lifecycle operation |
| `beginView` | `d3d9EndView` | Reversed operation |
| `endView` | `d3d9DrawSprite` | Wrong callback and signature |
| `applyProjection` | `d3d9DrawSpritePart` | Wrong callback and signature |
| `beginGUI` | `d3d9DrawRectangle` | Wrong operation |
| `setGuiProjection` | `d3d9DrawLine` | Wrong operation |
| `endGUI` | `d3d9DrawLineColor` | Wrong operation |
| `drawSprite` | `d3d9DrawText` | Wrong callback |
| `drawSpritePart` | `d3d9Flush` | Wrong callback |
| `drawSpritePartColor` | `d3d9CreateSpriteFromSurface` | Incompatible signature |
| `drawSpritePos` | `d3d9DeleteSprite` | Incompatible signature |
| `drawRectangle` | `NULL` | Required callback absent |

The old 16th entry is null and lands in modern `drawRectangle`, which the core validator requires. Earlier non-null entries are also unsafe: non-null does not mean correctly typed or semantically mapped. If the validator is bypassed, the miswired calls can invoke functions with incompatible signatures and corrupt execution.

**Implementation recommendation**
1. Replace the aggregate initializer with zero-initialization plus explicit member-name assignments (the audio and filesystem adapters already use named assignments).
2. Implement the modern renderer lifecycle in order: frame begin/end, view begin/end, projection, GUI begin/projection/end, then sprite/primitive drawing.
3. Leave genuinely unsupported optional entries null only where the shared core contract explicitly permits it. Never assign a legacy function to a merely similar-looking modern slot.
4. Add a renderer-vtable contract test that checks required members and a compile test that treats incompatible function-pointer assignments as errors.
5. Keep `Runner_create()` validation. Do not weaken the core validator to conceal the backend mismatch.

**Required test sequence**
- Assert every required callback points to the intended implementation, not merely a non-null address.
- Exercise frame/view/GUI callback order against the lifecycle in `Runner_drawPre`, `Runner_beginFrame`, `Runner_drawViews`, `Runner_drawPost` and `Runner_drawGUI`.
- Construct the runner and initialize its first room.
- Render a textured quad, rectangle, line, glyph and alpha-blended sprite before trying a full game.

### 8.4 Additional D3D9 renderer risks

These are source-level risks and validation gaps, not observed console failures.

**Initialization has no reliable failure contract.** `d3d9Init()` is a `void` callback. Shader compilation can fail and return early; shader object creation, vertex declaration creation, texture creation and texture lock/unlock operations are not all checked and propagated consistently. The TXTR upload path can continue after decode or texture creation failure, leaving missing/null texture state for later rendering. The constructor also needs to guarantee safe behavior if its own allocation fails.

**Recommendation:** add an explicit renderer-ready/failed state, check every allocation and HRESULT before using the result, and centralize idempotent cleanup for partially initialized resources. Because the shared `RendererVtable.init` currently returns `void`, do not silently change only the XDK implementation's signature. Either validate backend readiness before `Runner_create`, or make a coordinated shared-contract change that reports initialization status.

**Texture layout requires actual Xbox validation.** The code decodes PNG data and uploads to `D3DFMT_LIN_A8R8G8B8`; a source comment suggests direct sampling of linear textures, but the code does not perform the tiled copy described in that comment. This may be valid for the selected XDK usage, but the comment alone is not proof. Confirm the exact format, pitch, alignment, lock and sampling rules against the targeted XDK documentation and hardware.

Test 1x1, odd-width, non-power-of-two and large textures; distinct RGBA channel values; transparent edges; pitch/alignment boundaries; and both sprite and font sampling paths. Check all creation/lock/unlock/draw results.

**Primitive support is assumed.** `flushBatch()` uses `DrawPrimitiveUP(D3DPT_QUADLIST, ...)`, described in a comment as an Xbox 360 extension. The generic renderer interface also exposes points, lines, line strips, triangles, triangle strips and triangle fans. The legacy quad path does not implement that broader primitive contract. Verify QUADLIST against the exact Xbox API version; if unavailable, split each quad into two supported triangles. Test winding, UV orientation, alpha blending and batch boundaries (1, 2048 and 2049 quads).

### 8.5 Audio adapter: vtable wiring is good, but feature and failure semantics need tests

**Confirmed positive finding:** `xaudio2_audio.cpp` initializes its modern audio vtable by named member assignments (around lines 840–874), avoiding the renderer's positional-layout mistake. The constructor now checks both the object and instance-array allocations (lines 879–892). The adapter supports decoder-backed WAV/OGG playback, XAudio2 voices, gain/pitch, looping, pause/resume, audio groups and lazy AUDO loading.

**Remaining gaps**
- `xdkCreateStream`, `xdkDestroyStream` and `xdkPlayEncoded` are explicit unsupported stubs; they return failure/no-op results (around lines 820–833). Each core caller must be checked to ensure those results are handled without invalid stream use.
- Spatial sound and listener-position operations are stubs; this backend is not equivalent to a spatial audio implementation.
- `xdkAudioInit()` can return early when `XAudio2Create` or mastering-voice creation fails. The backend has an `initialized` flag, but `main.cpp` does not check that flag before continuing into first-room/game execution. Startup should either expose initialization failure through a supported contract or deliberately support a clearly logged silent-audio mode.
- OGG/WAV decode to PCM creates a memory-pressure risk with large AUDO assets and many simultaneous sounds. Measure peak decoded memory and voice count on actual game workloads.
- Validate SOND pitch versus runtime pitch semantics and the supported XAudio2 frequency-ratio range.
- Confirm each voice is stopped/destroyed before its PCM storage is freed, including error and shutdown paths.

**Audio test matrix**
1. Embedded WAV and OGG, plus external OGG file loading.
2. Empty, truncated, malformed and oversized WAV/OGG/AUDO data.
3. Mono/stereo and supported sample-rate variations.
4. Voice counts below, at and above `XDK_MAX_SOUND_INSTANCES`; verify the intended priority/eviction behavior.
5. Looping, pause/resume, stop, per-instance gain, master gain and audio-group gain, separately and combined.
6. Lazy group load, repeated load, failed load and shutdown with active voices.
7. Mock calls to unsupported stream/encoded APIs and verify graceful caller behavior.
8. Long-duration playback with memory and voice-reclamation checks.

### 8.6 Filesystem adapter: broad API coverage, but path and ownership contracts need tightening

**Confirmed positive finding:** `xdk_file_system.cpp` uses named assignments for all 20 modern filesystem callbacks. Whole-file reads/writes loop over partial Win32 operations and fail on zero progress; the implementation checks byte counts, rejects files beyond the engine's signed 32-bit size range, and handles the `SetFilePointer` invalid-sentinel case by consulting `GetLastError`.

**Additional risks**
- `xdkResolvePath()` roots ordinary relative paths under the configured base directory and converts slash styles, while allowing drive-qualified and root-leading paths through as absolute. It does not normalize or reject `..` traversal segments. Decide whether GameMaker-compatible absolute paths are intentionally allowed; then test and document the policy. If relative paths must remain inside the game directory, normalize and reject escapes.
- Directory enumeration silently skips an entry if allocating its name fails. That is only acceptable if callers are explicitly allowed to receive incomplete results; otherwise surface an error or define a deterministic failure policy.
- The 32-bit stream position/size contract must reject values that cannot be represented rather than wrap or truncate.
- The caller's ownership of each returned directory-entry name and the returned array must match the allocator/free convention used by the runner. Verify this against every caller to avoid leaks or mismatched frees.
- Validate drive roots, `game:\\`, `d:\\`, nested paths, empty relative paths, trailing separators, mixed slash styles, missing/read-only files, cross-directory rename and mixed directory contents.

**Filesystem test matrix**
1. Whole-file read/write: empty, one byte, multi-megabyte and boundary-size files.
2. Streaming open/read/tell/seek/write/close; start/end seeks and invalid negative offsets.
3. Partial reads/writes and zero-progress/error handling.
4. Rename/delete for existing, missing, read-only and cross-directory files.
5. Nested directory create/list/delete, including mixed files and subdirectories.
6. Relative, absolute, mixed-separator and `..` paths.
7. 32-bit boundary behavior and handle-leak checks on every error path.

### 8.7 Main startup, timing and controller integration

**Constructor results are not checked consistently.** `main.cpp` creates the filesystem, renderer, VM, audio system and runner, then immediately uses them. Add explicit checks for `XdkFileSystem_create`, `D3D9Renderer_create`, `VM_create`, `XdkAudioSystem_create` and `Runner_create`, with a single cleanup path that safely handles partial construction. Also check the CONFIG.JSN-derived controller-mapping allocation before writing into it.

**Performance-counter handling remains unsafe.** Both `_xdk_monotonic_ms()` (around line 30) and main-loop initialization call `QueryPerformanceFrequency` without checking success or verifying `freq.QuadPart > 0`. The loop divides by that value. Query once, validate it, and use a documented fallback clock or abort cleanly. Check `QueryPerformanceCounter` results too.

**Input is currently keyboard-emulated.** The XInput loop maps button edges into `RunnerKeyboard_onKeyDown/Up`; left-stick directions are converted to D-pad bits. This is useful compatibility behavior but is not native GameMaker gamepad parity. The adapter should also populate the modern `RunnerGamepad` state for `gamepad_*` built-ins: buttons, axes, triggers, deadzones, connect/disconnect and reconnect. Test edge transitions and neutralize stale state on disconnect. Keep keyboard mapping as a separate compatibility layer.

**Startup failure mode:** missing or invalid `data.win` currently enters an infinite hang. That may help debugging, but production behavior should log a clear error, unwind resources and exit; make the hang opt-in for debugger sessions.

**Ownership audit:** document which function allocates, initializes and destroys each subsystem. Compare the manual shutdown order in `main.cpp` against `Runner_free()` and adapter destroy functions to prevent double destruction or freeing resources still referenced by the runner.

### 8.8 XDK project/build configuration findings

The current `Butterscotch360.vcxproj` uses `ToolsVersion="4.0"`, Debug/Release `Xbox 360` configurations, forced inclusion of `compat_msvc.h`, compiles the listed C sources as C++, disables exceptions/RTTI, and links XDK-specific library variants. It does **not** declare a `PlatformToolset`. Therefore the project file alone does not identify the compiler, XDK release or installed Xbox platform targets; the matching licensed XDK/Visual Studio integration must provide them.

The project contains an empty `<ImageXex>` element and deployment metadata using `RemoteRoot=xe:\\Butterscotch`. Confirm that the imported XDK targets provide the intended linker and XEX packaging defaults. A successful MSBuild evaluation or compile step is not proof that a valid deployable XEX was produced.

**Build validation recommendations**
- Parse the project XML in CI and verify every explicit source path/glob resolves to tracked files.
- Validate Debug and Release dependency lists independently.
- Detect missing Xbox platform props/targets/compiler early and emit a targeted error.
- Record evaluated MSBuild properties and compiler/linker command lines in local build logs.
- Inspect the resulting XEX for expected entry point, imports and packaging metadata.
- Track portable CI, XDK compilation, Xenia execution and physical-console execution as separate validation gates.

### 8.9 Prioritized second-pass implementation plan

1. **P0 — repair renderer vtable wiring** using named assignments and correct modern callback semantics; add contract/signature/lifecycle tests.
2. **P0 — add safe subsystem construction and initialization failure handling** before first-room startup.
3. **P1 — implement the minimum modern renderer lifecycle** (frame/view/projection/GUI/sprite/primitive) and make partial initialization unwind safely.
4. **P1 — validate texture format/pitch/sampling and primitive support** against the exact Xbox API and target hardware.
5. **P1 — test filesystem path policy, ownership, stream boundaries and failure cleanup.**
6. **P1 — test audio unsupported paths, decode memory, voice lifecycle and init failure behavior.**
7. **P1 — add native RunnerGamepad bridging** while retaining keyboard emulation as a separate mapping.
8. **P2 — validate the actual XDK project integration and XEX packaging** in a properly licensed environment.
9. **P2 — execute separate Xenia and physical-console test plans**, recording exact game hash, runner SHA, build ID, scene and logs.
10. **Only optimize after correctness:** collect reproducible memory/frame/audio measurements before changing texture caching, VM speed or batching.

### 8.10 Second-pass confidence and limits

**Confirmed from current source:** renderer callback miswiring; modern renderer contract expansion; named audio/filesystem vtable assignment; audio stream/encoded/spatial stubs; current CONFIG.JSN safety checks; current Xenon endian macro condition; current XAudio2 constructor allocation checks; filesystem partial-I/O handling and path policy; missing performance-counter validation; project-file absence of an explicit PlatformToolset.

**Requires XDK/target validation:** Xbox-specific QUADLIST behavior; linear texture format/pitch/alignment and sampling; exact XAudio2 API/library behavior; XEX packaging defaults; actual compiler predefined macros and ABI details; gamepad behavior on console; storage path semantics.

**Not performed:** no XDK build, XEX generation, emulator test, physical-console test, game launch, profiler run or empirical audio/rendering test. This is a static source review and implementation plan, not a compatibility certification.
