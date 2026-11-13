# Undertale / Deltarune compatibility investigation

**Scope:** Game compatibility requirements for running Undertale and Deltarune in Butterscotch on Xbox 360. This report does not implement renderer, audio, filesystem, or platform code; it identifies the game-facing contracts those systems must satisfy.

**Research date:** 2026-11-13  
**Status:** Static source/documentation investigation. No game binaries were run, no Xbox 360 build was compiled, and no game was tested in this session.

## 1. Executive assessment

- **Undertale v1.08 is the best first target.** Its Windows build is GameMaker: Studio 1.4.1804 / bytecode (WAD) 16. The modern upstream explicitly names this build as its original compatibility target. The old Xbox 360 port reports that Undertale boots/plays, but with known defects (audio speed, performance, 4:3 output and Undyne spear aiming). This is useful evidence, not proof that the current migrated XDK branch works.
- **Deltarune's original 2018 SURVEY_PROGRAM is not a proxy for current Deltarune.** The original Chapter 1 demo is bytecode 16 and the old Xbox 360 README says this exact build does not boot. Later console builds are bytecode 17; the 2021 Chapter 1&2 release is documented as GameMaker Studio 2.3+. The paid Chapter 1–5 release has multiple independently versioned chapter data files.
- **WAD support is necessary but not sufficient.** Upstream advertises WAD 8–17 parsing, but explicitly warns that supported WAD versions do not guarantee game compatibility and the VM remains incomplete. Newer chapters exercise much more of the modern GameMaker resource/runtime surface than Undertale 1.08.
- **The current XDK renderer is a hard integration gate.** At the inspected fork commit, `src/xbox360-xdk/d3d9_renderer.cpp` still exposes the old 16-entry renderer vtable, while modern `Runner_create` validates dozens of required callbacks (surfaces, GUI, blending, shaders, texture operations, etc.). The fork's own migration notes say the D3D9 backend is pending and the XDK target is not build-verified. Consequently, this report's game-compatibility ratings are conditional on completing that platform integration.
- **Chapter 3 has a known non-cosmetic video requirement.** Its Tenna/VHS intro uses GameMaker's video display path; a PC hotfix added a skip/recovery path because some systems crash or freeze while displaying it. The migrated XDK build currently selects the null video backend, which creates no decoder. Chapter 3 therefore needs either a working video backend or reliable execution of the game's skip/fallback path.
- **The most defensible roadmap is:** (1) Undertale v1.08/WAD16; (2) Deltarune Chapter 1&2; (3) Chapters 3&4; (4) Chapter 5 and later releases. Validate each chapter's exact data file separately.

## 2. Inspected repository baselines

| Repository | Ref inspected | Exact commit SHA | Notes |
|---|---|---|---|
| Modern upstream | `ButterscotchRunner/Butterscotch` HEAD | `0c94bfb05e3cb4b8946a38f8085747035dd37f94` | README, WAD parser, VM, runner, renderer/video contracts inspected |
| Original Xbox 360 port | `ceilingtilefan/Butterscotch-360` HEAD | `1fd7659a4b95e0ce9837c3162de976cfc9ceec5a` | README and XDK D3D9 implementation inspected |
| Working fork | `Viniqpp/Butterscotch-360-2` branch `refresh2/modern-upstream-migration` | `39fc2d4b9c5163f7d1a931a743e8767991974a6c` | Branch ref verified before report creation |

The inspected fork commit's parent is `fa64392a471f20878f712d2d4253a66e01cfd2ba`. The branch includes the modern core migration and XDK adapter work documented in `docs/XBOX360_REFRESH2_MIGRATION.md`. That migration document records the renderer as pending and says the XDK changes have not been compiled with the Xbox 360 toolchain.

**Important reproducibility note:** game version strings and data.win MD5s below come from UndertaleModTool's published version reference. The actual Undertale/Deltarune game data files were not available in either inspected repository and were not downloaded or parsed here. WAD/build-version assignments are called out as verified, inferred, or unknown rather than guessed.

## 3. GameMaker runtime and WAD/data format

### 3.1 What the runner accepts

The upstream README lists support for:

- WAD 8 — GameMaker: Studio 1.0.198+
- WAD 9 — 1.0.527+
- WAD 10 — 1.1.609+
- WAD 11 — 1.1.754+
- WAD 12 — 1.1.867+
- WAD 13 — 1.1.917+
- WAD 14 — 1.4.1464+
- WAD 15 — 1.4.1675+
- WAD 16 — 1.4.1767+
- WAD 17 — GameMaker Studio 2.2+

Upstream clarifies that UndertaleModTool often calls this field the *bytecode version*, while Butterscotch calls it the WAD version. WAD is the version field in the GEN8 header; it is not a guarantee that every resource format, bytecode instruction, built-in or extension used by a game is implemented.

**Fork source evidence:**
- `src/wad_versions.h`: compile-time gates `ENABLE_WAD14`, `ENABLE_WAD16`, `ENABLE_WAD17`; WAD15 is handled in the WAD16 family.
- `CMakeLists.txt`: options enable WAD14, WAD16 and WAD17 by default.
- `src/data_win.c:~289-405`: GEN8 header and version parsing.
- `src/data_win.c:~540-580`: extension chunk layout changes for WAD17 and GameMaker 2022.6 / 2023.4+.
- `src/data_win.c:~646-775`: audio/SND and audio-group format handling, including newer GameMaker 2024.13/2024.14 differences.
- `src/data_win.c:~1447-1472`: shader resource version and console shader variant fields for WAD versions above 13.
- `src/data_win.c:~1502-1555`: WAD17 font optional fields.
- `src/vm.c`: WAD15/16 and WAD17-specific variable, array, argument and stack handling.

### 3.2 Undertale

**Verified:** Undertale v1.08 Windows/Steam is bytecode/WAD 16, GameMaker 1.4.1539 according to the UndertaleModTool version table. The Butterscotch README's historical target says Undertale v1.08 / GameMaker 1.4.1804 / bytecode 16. This apparent 1.4.1539 vs 1.4.1804 discrepancy reflects platform/build variants and the older README's chosen build target; use the actual supplied `data.win` GEN8 header as the final authority.

Useful published v1.08 Windows/Steam data.win MD5: `5903fc5cb042a728d4ad8ee9e949c6eb`. Other v1.08 platforms have different files/checksums. Do not assume the checksum matches every PC distribution.

WAD16 is supported by the modern parser when WAD16 is enabled. That makes Undertale a format-compatible candidate, not a confirmed current-fork pass.

### 3.3 Deltarune versions

| Build | Published runtime information | WAD/bytecode status | Compatibility significance |
|---|---|---|---|
| Chapter 1 `SURVEY_PROGRAM` (2018 Windows demo) | GameMaker Studio 2; bytecode 16 | **Verified bytecode 16 / WAD16** | Old Xbox 360 README explicitly says this Deltarune build does not boot. |
| Chapter 1 console release (2019) | GameMaker Studio 2 | **Verified bytecode 17 / WAD17** | Not the same binary as SURVEY_PROGRAM. |
| Chapters 1&2 combined PC release (2021) | GameMaker Studio 2.3+ | **WAD17 expected from the GMS 2.3+ generation; inspect actual GEN8 before certifying** | More recent VM/resource model; separate game data from the 2018 demo. |
| Paid Chapter Select / Chapters 1–4 (2025) | Per-chapter data.win files; chapter versions are tracked separately | Exact bytecode/WAD not stated in the published version table; inspect each data.win | Chapter 3 adds video playback and rhythm-game/audio edge cases. |
| Chapter Select / Chapters 1–5 (2026) | Per-chapter data.win files; current catalogued Windows build includes Chapters 1–5 | Exact bytecode/WAD not stated in the published version table; inspect each data.win | Newer chapter assets and fixes; Chapter 5 is a separate 2026 game-data target. |

UndertaleModTool's current reference lists these Windows Steam data.win MD5s for the Chapter Select package at its 2026-07-31 snapshot:

| Component | Game version | MD5 |
|---|---:|---|
| Chapter Select | v23 | `83a5a14f9b92a20f21fb9ec6c8528469` |
| Chapter 1 | 1.43 | `0ccbfd7c4f9fb1b86de1e2aaec0bacc9` |
| Chapter 2 | 1.52 | `1592c9bffa2d9e53ddeedc0c4f9a07d6` |
| Chapter 3 | 0.0.105 | `b43158db2e958e767ebb1aae72fb05a1` |
| Chapter 4 | 0.0.110 | `27e36f883f4ade21707dc8261072d416` |
| Chapter 5 | 0.0.253 | `9c80e6300e0548d933cc006f3c22760d` |

These hashes are identification aids, not a claim that no later patch exists after the reference snapshot. The Chapter Select executable and individual chapter data files should be treated as separate test inputs. The published history shows that the Windows Chapter Select package has been updated repeatedly, while Chapter 1–5 data versions can change independently.

### 3.4 Practical format conclusion

- WAD16 coverage is the minimum for Undertale v1.08 and the original Deltarune demo.
- WAD17 coverage is required for later Deltarune targets, including the 2019 console build and expected GMS 2.3+ data.
- The modern parser contains post-WAD17/GameMaker 2022–2026 resource-layout branches. These are evidence of parser support work, not proof that all chapter data structures or runtime features are complete.
- Do not test only the Chapter Select `data.win`. Extract and test each chapter's data.win, then test the chapter-selection/save-transfer flow separately.

## 4. VM, bytecode, built-ins, extensions and data structures

### 4.1 Required VM semantics

The runner must correctly execute the game's bytecode, not merely parse the WAD header. Priority VM areas:

1. **Bytecode dispatch and operand decoding:** WAD16 instructions for Undertale; WAD17 instruction/operand variants for later Deltarune.
2. **Variable scopes and built-in variables:** self/other/global/local, instance IDs, object references, arrays of built-in variables, and GameMaker's dynamic variable behavior.
3. **Function calls and call frames:** script functions, event functions, argument handling, return values, method binding, and nested calls.
4. **Array semantics:** array creation, indexing, nested arrays, copy-on-write, ownership/refcount behavior, and array values passed through functions.
5. **Structs and methods:** modern GML structs, method values/binding, constructor behavior and dynamic field access are especially important for newer GameMaker 2.x games.
6. **Control flow and arithmetic:** comparisons, short-circuit behavior, conversions, bitwise operations, string concatenation, random-number behavior, and correct integer/real coercion.
7. **Instance lifecycle and events:** create/destroy/cleanup, alarms, room transitions, inherited events, draw/GUI events and asynchronous event dispatch.
8. **Data structures:** arrays and GML structs plus legacy DS maps/lists/queues/stacks/grids/priority queues where the game calls them. Correct creation, lookup, iteration, deletion and cleanup semantics matter; returning dummy values may let a title boot but will break gameplay.
9. **Persistence and serialization:** save/load logic depends on file functions and exact string/number behavior, not only the filesystem adapter.

The fork's modern `src/vm.c` explicitly contains WAD17-specific argument handling (`argument0`–`argument15`), `INSTANCE_ARG`, `INSTANCE_STACKTOP`, array copy-on-write paths and struct access helpers. Relevant source anchors include `src/vm.c:214-270`, `302-327`, `684-722`, `913-1009`, and `1140-1307`. These are critical correctness paths for WAD17 and should receive targeted regression tests.

The source tree also contains dedicated `src/gml_array.c/.h`, `src/gml_method.c/.h`, integer/RValue hash maps, and `src/vm_builtins.c/.h`. This is a substantially more modern runtime than the old Xbox port's VM. A WAD17 boot test should include array and method behavior, not only a title screen.

### 4.2 Built-in function families to validate

This is a compatibility test inventory, not a claim that every listed API is statically imported by every chapter.

| Built-in family | Undertale | Deltarune 1–2 | Later chapters | Why it matters |
|---|---|---|---|---|
| Core math, strings, conversions, comparisons, random | P0 | P0 | P0 | Used throughout game logic, combat, text and procedural patterns. |
| Instance/object/room/event APIs | P0 | P0 | P0 | Core GameMaker game loop and state transitions. |
| Arrays, structs, methods, argument access | P1 | P0 | P0 | WAD17/GMS2+ code relies on modern data semantics. |
| Legacy DS list/map/stack/queue/grid/priority queue APIs | P1 | P1 | P1 | Required wherever called; verify full lifetime and iteration semantics. |
| File and INI APIs | P0 | P0 | P0 | Saves, configuration, chapter completion/transfer files and persistent state. |
| Drawing, fonts, sprite, texture and GUI APIs | P0 | P0 | P0 | Dialogue, menus, battle UI, effects and cutscenes. |
| Surface/application-surface APIs | P1 | P1 | P1 | Off-screen rendering, compositing, transitions and texture capture. |
| Shader/GPU APIs | P1 | P1 | P1 | GameMaker shader resources and per-layer effects; actual chapter use must be enumerated. |
| Audio APIs (play, stop, gain, pitch, loop, group) | P0 | P0 | P0 | Music timing, combat cues, ambience and rhythm events. |
| Video APIs | — | — | P1 for Chapter 3 | Tenna VHS scene is documented to use GameMaker's video display method. |
| Gamepad APIs | P1 | P1 | P1 | Controller navigation and gameplay; the XDK input bridge must populate GameMaker gamepad state, not only keyboard events. |
| Extension / external function APIs | Verify | Verify | P1 investigation | Game-specific external extension calls can be a hard failure even when the core VM is correct. |

**Extension caveat:** Upstream README says FMOD is the only implemented extension and warns that games using other external extensions (including shader-related extensions) may fail. Deltarune's Steam depot includes Windows DLLs such as `execute_program_pipe_x64.dll` and `shader_replace_simple_x64.dll` in the Chapter 5 package. Their presence alone does not prove that a given game data.win invokes them, or that they are mandatory for gameplay. Inspect the EXTn chunk and decompile extension calls for each chapter before deciding whether to stub, emulate, or omit them.

## 5. Audio, video, input and filesystem game requirements

### 5.1 Audio

**Undertale**
- Music and sound effects are fundamental. Validate correct sample rate/pitch and timing, not just that a sound is audible.
- The original Xbox port documents some audio playing too fast. This is a known legacy regression to retest.
- The original port's instructions mention optional external `.ogg` files for additional audio. Ensure external audio paths and OGG decoding work, and confirm the game behaves correctly when a sound is embedded versus external.

**Deltarune**
- Music-synchronized events are more demanding than ordinary background music. Chapter 3 has rhythm-game segments; a published hotfix added a failsafe for systems with no audio output.
- Later release notes document a softlock caused by disconnecting an audio device during music-synchronized sequences. This makes pause/resume, device loss, playback position and timing behavior compatibility concerns, not polish.
- Chapter 5's Steam depot contains `audiogroup1.dat` and additional OGG assets. Audio-group lazy loading and external path resolution must be verified.

**Fork evidence / gaps**
- `src/xbox360-xdk/xaudio2_audio.cpp` adapts the modern audio vtable and reads modern audio flags/groups. The migration notes state that the current XDK audio adapter does not yet implement spatial routing, stream voices, encoded playback or seeking; several callbacks safely no-op/fail.
- `xdkSetTrackPosition` is explicitly a TODO; `xdkSetChannelCount` is a no-op.
- Affected games may still boot with these gaps, but music-synced timing and seek/loop behavior can fail. Chapter 3+ should not be considered compatible until audio-device and rhythm-event tests pass.

### 5.2 Video

- Upstream has a video abstraction in `src/video/video.h` with open/close, audio extraction, pause/resume, loop, clock, update, draw, duration and position callbacks.
- The migrated XDK project selects `src/video/null_video.c`; its backend factory returns `nullptr`. There is therefore no XDK video decoder in the inspected branch.
- Chapter 3's Tenna VHS intro is the known critical case. The PC hotfix explicitly added an option to skip the video after a crash/freeze. A null backend may allow a game fallback only if the game's logic handles unavailable video; this must be tested, not assumed.
- Chapters 1, 2, 4 and 5 should still be tested for video API calls and other media playback. No claim is made here that they contain no video dependency.

### 5.3 Input and controller

- The XDK main loop polls XInput and maps controller buttons/stick directions to GameMaker keyboard key-down/key-up events.
- This supports keyboard-style navigation mappings, but the inspected `src/xbox360-xdk/main.cpp` does not populate the modern `RunnerGamepad` device state. GameMaker `gamepad_*` calls may therefore see no connected gamepad even while XInput is physically connected.
- The modern core has `src/runner_gamepad.c` APIs for device count, connection, button state, axis state, deadzone and threshold. The platform bridge needs to feed this state if titles use native gamepad APIs.
- Deltarune's published console patch history includes controller calibration and disconnect-related fixes. The game should be tested with analog sticks, D-pad, face buttons, menu navigation, disconnect/reconnect and simultaneous inputs.
- Test button mapping and text/dialogue confirmation separately: the legacy mapping A→Enter, B→Shift, X→Ctrl, Y→'X', Start/Back→Escape is a compatibility shim, not full native controller parity.

### 5.4 Filesystem and save data

- Undertale requires reliable save/config read/write and directory/file behavior.
- Deltarune requires multiple chapter-specific save/completion records and cross-chapter data transfer. Chapter 1/2 and later paid releases have distinct data versions; save compatibility is separate from WAD compatibility.
- The Deltarune version notes document chapter-completion data being transferred from Chapter 1 to Chapter 2, and later changes to completion-file checks and chapter-specific inventory/reward state.
- The fork's XDK filesystem adapter implements the modern filesystem vtable, including text/binary I/O, rename/delete, streaming handles and directory enumeration, according to `docs/XBOX360_REFRESH2_MIGRATION.md`. This is source-level implementation evidence only; XDK behavior has not been runtime-tested.
- Verify filename case behavior, path separators, current working directory, relative paths, write atomicity, file size limits, directory creation, and save recovery. Do not treat a successful initial save as proof of chapter transfer correctness.

## 6. Rendering, shaders, surfaces and texture requirements

### 6.1 Game-facing rendering features

Both games need correct 2D sprite and font rendering, alpha blending, color tinting, clipping/cropping, texture-atlas UVs, draw order/depth, GUI-space rendering, room/view transforms and consistent frame timing.

Higher-risk features include:
- Application surface and custom surfaces, render-target switching, surface copy/resize/free, reading pixels and converting a surface to a sprite.
- Blend mode variants, alpha testing, color write masks, texture filtering and texture-stage selection.
- Shader resources, uniform/sampler lookup and per-layer shader assignment.
- View rotation, viewport scaling and widescreen/letterbox behavior.
- Sprite sequence resources and newer sprite metadata fields in newer GameMaker builds.

The modern renderer contract in `src/renderer.h` exposes surface, texture, shader, GUI and GPU state callbacks. `src/runner.c:2470-2534` validates many of these callbacks during runner creation. This means renderer support is not optional merely because a game is mostly 2D.

### 6.2 Current XDK renderer status

At the inspected fork commit, `src/xbox360-xdk/d3d9_renderer.cpp:721-739` initializes a legacy, short renderer vtable (16 entries). Modern `RendererVtable` has a much larger contract; modern `Runner_create` requires methods including GUI passes, surface lifecycle, render-target binding, shader uniforms, blend modes, texture metadata and other GPU state operations.

The D3D9 file has working-looking basic sprite/rectangle/line/text draw paths and uploads TXTR pages through PNG decode to `D3DFMT_LIN_A8R8G8B8`. It also explicitly has:
- `d3d9CreateSpriteFromSurface` returning `-1` with a TODO for surface capture.
- `d3d9DeleteSprite` as a TODO.
- View rotation marked TODO.
- No implementation in the shown vtable for the modern surface/shader/GUI/GPU callbacks.

**Compatibility consequence:** this renderer is not yet a valid modern renderer backend. Do not rate any game as playable on the current XDK branch until the vtable is reconciled and runner startup succeeds. Once it is, implement features based on actual GML calls and fail loudly for unsupported required operations rather than silently drawing incorrect output.

### 6.3 Xbox 360 GPU constraints

- Xbox 360's Xenos GPU uses tiled texture layouts for normal textures; the current code comments describe linear texture staging and direct sampling. Confirm this path with actual XDK texture allocation and sampling. The comment says a tiled copy should be created, but the implementation assigns the linear texture directly.
- The current renderer uses point filtering, suitable for pixel art but potentially wrong for game sections that expect interpolation.
- `D3DFMT_LIN_A8R8G8B8` allocation may impose size/alignment restrictions. Large atlas pages and surface dimensions must be checked against Xenos constraints.
- Surface readback/capture and shader translation are likely harder than simple sprite rendering.
- GameMaker shader source is not automatically compatible with Xbox 360 HLSL profiles. The existing simple HLSL sprite shader is a platform blit shader, not proof that arbitrary GameMaker shaders or shader extensions work.
- Xbox 360 memory is shared and constrained. Avoid unnecessary full-size surface copies and unbounded decoded texture caches; profile Chapter 3–5 content separately.

## 7. Chapter-specific compatibility differences

### Undertale v1.08
- Older GMS 1.4 / WAD16 target, and the stated historical goal of Butterscotch.
- Old Xbox 360 port reports that it can run, with audio speed, performance, aspect-ratio and spear-aiming defects.
- Best first target for validating VM16, standard audio, save I/O, sprite/font drawing, battle UI, room transitions and controller-to-keyboard mapping.
- Undertale's WAD16 success does not validate WAD17 arrays/structs or the newer Deltarune runtime.

### Deltarune Chapter 1 (2018 SURVEY_PROGRAM)
- WAD16 / bytecode 16, but the old Xbox 360 README explicitly says it does not boot.
- This is a useful early failure reproducer. Capture the startup failure point and VM/parser logs before attempting later chapters.
- Do not confuse it with the later Chapter 1 data included in the 2021 combined release or the 2025 paid Chapter Select build.

### Deltarune Chapters 1–2 (2021 combined release and later paid versions)
- Built with GMS 2.3+ according to the published version history; treat WAD17 as the expected target but verify the actual GEN8 field in the supplied file.
- Requires the newer VM paths: arrays, structs/methods, argument access and WAD17 variable semantics.
- Chapter 1 → Chapter 2 continuity requires reliable save/completion files and inventory/state transfer.
- Console history includes controller behavior and display-border/TV-format fixes, so controller input and aspect-ratio rendering should be part of the test matrix.
- Music-synchronized sequences require stable frame pacing and audio position.

### Deltarune Chapter 3
- Separate chapter data file and version history.
- Known additional requirements: Tenna/VHS video playback; rhythm-game timing/audio; substantial combat/action sequences.
- Video backend absence is a direct risk. Audio-device failure/disconnection is also known to have caused game-side softlocks in later music-synchronized content.
- Prioritize video fallback behavior, audio timing, texture/surface transitions around the VHS scene, and stress tests for bullet patterns.

### Deltarune Chapter 4
- Separate chapter data file and release line; do not assume Chapter 3's exact content or runtime calls.
- Published notes include console-specific minigame/input variations, so controller and display-path behavior still matter.
- Validate its exact data.win and resource chunks independently, including any new sequence, shader, surface or extension calls discovered by decompilation.

### Deltarune Chapter 5
- Added in 2026; it has its own data.win and audio-group payload. The 2026 Steam depot includes Chapter 5 data.win and audiogroup assets.
- Treat as a new compatibility target, not merely a larger Chapter 4. Its game version and content change independently from the Chapter Select executable.
- The current version catalog snapshot lists Chapter 5 v0.0.253. Recheck the data.win checksum against the actual test build before reporting results.
- Newer GameMaker data parsing in the current runner includes GameMaker 2024.13/2024.14+ audio-group, sprite-sequence, tile and resource-layout branches. This is evidence of newer-format support in the parser, but the exact Chapter 5 file's GEN8/build values still need to be read directly.
- Chapter 5 compatibility depends on the full WAD17+ runtime and all chapter-specific resources/extensions; no successful run is claimed.

## 8. Known incompatibilities and likely failure points

### Verified from repository/source documentation

1. Upstream says WAD8–17 are supported but explicitly warns that bytecode support remains WIP and WAD compatibility does not guarantee game compatibility.
2. Upstream says external extensions other than FMOD may fail; extension support is not general-purpose.
3. Original Xbox 360 README reports Undertale audio-speed problems, slowdowns, poor 4:3 optimization and incorrect Undyne spear aiming.
4. Original Xbox 360 README reports Deltarune SURVEY_PROGRAM does not boot.
5. Current fork migration notes say the XDK target has not been compiled with the Xbox 360 XDK and the modern D3D9 renderer remains pending.
6. Current XDK video backend is null.
7. Current XDK audio adapter lacks seek, spatial routing, stream voices and encoded playback; channel count is a no-op.
8. Current XDK input bridge injects keyboard events from XInput but does not feed modern RunnerGamepad state.
9. Current D3D9 renderer lacks modern renderer vtable callbacks; surface-to-sprite capture and dynamic sprite deletion are TODOs.

### Hypotheses / unverified risks (must be tested)

- Some Deltarune code may depend on newer built-ins, extension calls or resource formats not exercised by Undertale v1.08.
- Newer chapter builds may require GameMaker video functions beyond the Chapter 3 VHS sequence.
- Deltarune's music synchronization may expose timing/seek/device-loss problems even if ordinary OGG playback sounds correct.
- Shader replacement DLLs shipped in a Steam depot may indicate extension-dependent behavior, but their presence does not prove they are called by every chapter or required for ordinary gameplay.
- Large Chapter 3–5 texture pages, surfaces, and audio groups may exceed current memory/performance assumptions on Xbox 360.
- Save-file naming and path assumptions may differ from desktop. File transfer between chapter saves could fail even if each chapter starts independently.

## 9. Prioritized compatibility matrix

Legend: **P0** = blocker / required for basic play; **P1** = required for full gameplay or specific chapter features; **P2** = optional, cosmetic or title-dependent; **Unknown** = needs static extraction or runtime confirmation.

| Feature / requirement | Undertale v1.08 | Deltarune Ch. 1–2 | Deltarune Ch. 3–5 | Priority / notes |
|---|---|---|---|---|
| WAD16 parser + VM | Required | Required for original 2018 demo; later builds are newer | Baseline only | P0 for Undertale and original demo |
| WAD17 parser + VM semantics | Not needed for standard PC v1.08 | Required/expected for GMS2.3+ builds | Required/expected | P0 for later Deltarune; verify GEN8 |
| Correct variables, calls, events, rooms, alarms | Required | Required | Required | P0 |
| Arrays / copy-on-write / nested arrays | Required as used by game | Required | Required | P0/P1; validate against real bytecode |
| Structs / methods / modern argument access | Low/unknown for 1.4 target | Required for GMS2.3+ runtime | Required | P0 for Deltarune |
| DS data structures | Required if called | Required if called | Required if called | P1; enumerate exact APIs from each data.win |
| Save/load and file/INI functions | Required | Required | Required, including cross-chapter state | P0 |
| Sprite/font/texture atlas drawing | Required | Required | Required | P0 |
| GUI, blending, alpha, view transforms | Required | Required | Required | P0 |
| Surfaces / application surface / render targets | Required if called; high likelihood | Required if called | Required if called | P1; current D3D9 backend not ready |
| Shader resources / uniforms / samplers | Verify actual calls | Verify actual calls | Verify actual calls; extension risk | P1/P2 title-dependent |
| Audio decode, groups, OGG, pitch, loop, timing | Required | Required | Required; music/rhythm critical | P0 |
| Audio seeking / device loss / stable clock | Useful; legacy speed bug exists | Important for synced sequences | High priority | P1 |
| Video decoder | No known requirement | No known requirement | Chapter 3 VHS confirmed risk; other chapters unknown | P1 for Ch. 3 |
| Native gamepad state | Desirable; keyboard fallback exists | Important | Important | P1; current XDK maps only to keyboard |
| External extension emulation | Verify | Verify | High-priority inspection | P1 if invoked; only FMOD generally implemented upstream |
| High-performance instance/VM loops | Required for acceptable play | Required | Required; more demanding content | P1 |
| Correct 4:3/letterbox and scaling | Needed for correct presentation | Needed | Needed | P1; old Xbox port known issue |
| Surface capture / dynamic sprite deletion | Unknown/title-dependent | Unknown | Unknown | P2 unless data.win calls them |
| Cosmetic post-processing / CRT effects | Not assumed required | Not assumed required | Not assumed required | P2 |

### Suggested test order

1. **Parser smoke tests:** open each target data.win, log GEN8 WAD/build, parse all chunks, report unknown chunks/resource versions and extension names.
2. **VM smoke tests:** start Undertale v1.08; then run the 2018 SURVEY_PROGRAM as a regression reproducer; then test 2021 Chapter 1&2 and each 2025/2026 chapter file.
3. **Game logic tests:** intro, title/menu, first room, room transitions, battle start/end, inventory, save/load, reload and restart.
4. **Input tests:** keyboard mapping, native gamepad API calls, analog stick deadzones, D-pad, disconnect/reconnect and button edge behavior.
5. **Audio tests:** embedded and external OGG, groups, simultaneous effects, pitch, loop, pause/resume, seeking if invoked, music-synced events and device-loss recovery.
6. **Rendering tests:** GUI, fonts, atlas edges, alpha blending, views, 4:3 scaling, application surface, custom surfaces, render-target changes and shaders.
7. **Chapter-specific tests:** Chapter 3 VHS/video fallback and rhythm game; Chapter 4 minigames; Chapter 5 large asset/audio groups and save transfer.
8. **Long-session tests:** repeated room changes, menu reloads, save slots, memory pressure, audio group unload/reload, and no-growth checks for surfaces/textures/instances.

## 10. Recommended next investigation steps (game-only)

1. Obtain the exact PC Windows data.win for Undertale v1.08 and each Deltarune chapter build that will be used for testing.
2. Run UndertaleModTool on each file and record: GEN8 GameMaker major/minor/release/build, WAD/bytecode version, resource chunk versions, extension names/files, shader names, audio group names, sequence resources and video-related resources.
3. Decompile each game's code and generate a per-chapter list of all built-in function and variable references. Compare that list against `VMBuiltins_registerAll` and `VMBuiltins_getVariable/setVariable` in the inspected runner source.
4. Add targeted VM tests for each bytecode opcode/variable access form used by those binaries, especially WAD17 arrays, structs, methods and argument slots.
5. Keep a chapter-by-chapter test ledger with exact game version, data.win MD5, runner commit, platform build ID, last known good room/scene and logs. A single “Deltarune works” label is too broad to be meaningful.

## 11. Sources and technical references

### Project source and migration baselines

- [Modern Butterscotch upstream](https://github.com/ButterscotchRunner/Butterscotch/tree/0c94bfb05e3cb4b8946a38f8085747035dd37f94)
- [Modern upstream README: WAD versions, caveats, extensions and supported platforms](https://github.com/ButterscotchRunner/Butterscotch/blob/0c94bfb05e3cb4b8946a38f8085747035dd37f94/README.md)
- [Modern upstream data_win.c](https://github.com/ButterscotchRunner/Butterscotch/blob/0c94bfb05e3cb4b8946a38f8085747035dd37f94/src/data_win.c)
- [Modern upstream VM](https://github.com/ButterscotchRunner/Butterscotch/blob/0c94bfb05e3cb4b8946a38f8085747035dd37f94/src/vm.c)
- [Modern upstream VM built-ins](https://github.com/ButterscotchRunner/Butterscotch/blob/0c94bfb05e3cb4b8946a38f8085747035dd37f94/src/vm_builtins.c)
- [Original Xbox 360 port](https://github.com/ceilingtilefan/Butterscotch-360/tree/1fd7659a4b95e0ce9837c3162de976cfc9ceec5a)
- [Original Xbox 360 port README and known game issues](https://github.com/ceilingtilefan/Butterscotch-360/blob/1fd7659a4b95e0ce9837c3162de976cfc9ceec5a/README.md)
- [Working fork migration notes](https://github.com/Viniqpp/Butterscotch-360-2/blob/39fc2d4b9c5163f7d1a931a743e8767991974a6c/docs/XBOX360_REFRESH2_MIGRATION.md)
- [Fork WAD version gates](https://github.com/Viniqpp/Butterscotch-360-2/blob/39fc2d4b9c5163f7d1a931a743e8767991974a6c/src/wad_versions.h)
- [Fork renderer contract](https://github.com/Viniqpp/Butterscotch-360-2/blob/39fc2d4b9c5163f7d1a931a743e8767991974a6c/src/renderer.h)
- [Fork runner renderer validation](https://github.com/Viniqpp/Butterscotch-360-2/blob/39fc2d4b9c5163f7d1a931a743e8767991974a6c/src/runner.c#L2470-L2534)
- [Fork XDK D3D9 renderer](https://github.com/Viniqpp/Butterscotch-360-2/blob/39fc2d4b9c5163f7d1a931a743e8767991974a6c/src/xbox360-xdk/d3d9_renderer.cpp)
- [Fork XDK audio adapter](https://github.com/Viniqpp/Butterscotch-360-2/blob/39fc2d4b9c5163f7d1a931a743e8767991974a6c/src/xbox360-xdk/xaudio2_audio.cpp)
- [Fork XDK input/main loop](https://github.com/Viniqpp/Butterscotch-360-2/blob/39fc2d4b9c5163f7d1a931a743e8767991974a6c/src/xbox360-xdk/main.cpp)
- [Fork video interface](https://github.com/Viniqpp/Butterscotch-360-2/blob/39fc2d4b9c5163f7d1a931a743e8767991974a6c/src/video/video.h)
- [Fork null video backend](https://github.com/Viniqpp/Butterscotch-360-2/blob/39fc2d4b9c5163f7d1a931a743e8767991974a6c/src/video/null_video.c)

### Game version and behavior references

- [Undertale and Deltarune data.win version reference — UndertaleModTool](https://underminersteam.github.io/reference/undertale-deltarune-versions/)
- [Undertale bytecode and GameMaker version table](https://github.com/UnderminersTeam/UndertaleModTool/wiki/Undertale-versions-and-bytecode)
- [Deltarune version differences and Chapter 3 video/audio hotfix notes](https://deltarune.wiki/w/Version_Differences)
- [Deltarune official Steam listing](https://store.steampowered.com/app/1671210/DELTARUNE/)
- [Toby Fox release announcement for Chapters 1–4](https://toby.fangamer.com/newsletters/release-date/)
- [Deltarune Chapter 5 SteamDB depot/update record](https://steamdb.info/patchnotes/23736632/)

---

## 12. Confidence and limits

**Verified from sources:** repository SHAs, upstream WAD support list, Undertale v1.08 and original Deltarune demo bytecode versions, Deltarune chapter version/data.win hashes listed above, old Xbox port's documented failures, current XDK source gaps, Chapter 3 VHS/video hotfix, and the audio/controller/save-related release-note issues.

**Inferred:** WAD17 is expected for GMS2.3+ Chapter 1&2 builds; the later chapter builds are expected to use the modern WAD17-era format. Confirm each actual GEN8 header before treating this as fact.

**Not established:** exact built-in function call inventories per chapter; exact WAD/build numbers for the paid Chapter 3–5 data.win files; whether the Steam DLLs are called by each chapter; complete shader/surface/video usage per chapter; actual Xbox 360 runtime compatibility; XDK compilation success; frame rate and memory use.

No testing or compilation is claimed. This is a static compatibility investigation intended to guide the next binary-level audit and test plan.
