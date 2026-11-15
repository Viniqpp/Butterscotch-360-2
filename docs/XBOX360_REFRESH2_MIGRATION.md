# Butterscotch 360 Refresh 2 — Migration Audit

Status: partial source migration committed; not yet build-verified with the Xbox 360 XDK.

## Goal

Bring the Xbox 360 platform support from this repository forward to the current
Butterscotch runner, while retaining the Xbox 360 XDK target. The target is the
modern upstream runtime (including WAD 17-era changes), not a backport of the
old runner's limited feature set.

## Repositories and baselines

- Xbox 360 base: `ceilingtilefan/Butterscotch-360`
  - Base commit: `1fd7659a4b95e0ce9837c3162de976cfc9ceec5a`
- Modern upstream: `ButterscotchRunner/Butterscotch`
  - Inspected upstream commit: `0c94bfb05e3cb4b8946a38f8085747035dd37f94`
- Working branch: `refresh2/modern-upstream-migration`

The upstream README advertises WAD 8–17 support, while noting that WAD 17
support does not guarantee every game works and that the VM is still evolving.

## Why this is a port, not a simple file copy

The old Xbox 360 port is based on a substantially older runner API. The backend
contracts have changed significantly:

| Interface | Old Xbox port | Modern upstream | Consequence |
| --- | ---: | ---: | --- |
| Renderer vtable | 16 methods | 81 methods | D3D9 backend needs a deliberate implementation of the expanded contract. |
| Audio system vtable | 21 methods | 31 methods | XAudio2 backend needs new lifecycle, streaming, spatial and group hooks. |
| File system vtable | 5 methods | 20 methods | XDK filesystem needs binary I/O, streaming, rename and directory operations. |
| Runner | 125 lines | 1,051 lines | Runtime integration cannot be solved by swapping only platform files. |
| DataWin parser | 675 lines | 1,088 lines | Parser behavior and data structures must remain in sync with upstream. |
| RValue | 240 lines | 664 lines | VM value semantics and memory ownership have evolved. |
| VM | 197 lines | 400 lines | VM changes are a core compatibility dependency. |

These counts are from the inspected header/source snapshots and are intended
to show the scale of the API drift, not to serve as a permanent metric.

## Existing Xbox-specific code worth preserving

The old port contains platform code under:

- `src/xbox360/`
- `src/xbox360-xdk/`

The XDK target includes a D3D9 renderer, XAudio2 audio system, XDK filesystem,
main loop, MSVC compatibility shim, and Visual Studio/XDK project configuration.
These are useful reference implementations, but they must be adapted to the
modern interfaces rather than copied over the new core.

## Known legacy limitations to retest

The old port documents these issues:

- Audio can play at the wrong speed in some Undertale situations.
- 4:3 output is not properly optimized.
- Performance slowdowns occur.
- Undyne spear aiming is incorrect.
- Deltarune's `SURVEY_PROGRAM` does not boot.

These are regression targets, not assumptions about the modern port.

## Migration strategy

### 1. Establish a clean modern core baseline

**Source snapshot imported.** The working branch now contains 134 selected
source/dependency blobs from upstream commit `0c94bfb`, while preserving the
Xbox 360 XDK files. The legacy `main` branch has not been modified. This is the
runtime/core subset selected for the XDK integration, not a wholesale copy of
every upstream desktop/mobile backend or test asset.

### 2. Reconcile platform/build configuration

**Build-system migration committed.** The modern upstream `CMakeLists.txt`,
CLI entry point, core loop and no-op backend are imported. A GitHub Actions
headless/no-op smoke workflow is configured to compile the modern core on Linux.
The XDK `Butterscotch360.vcxproj` includes the modern root core, default font,
image decoder, FMOD compatibility sources, disabled-physics implementation,
null-video backend and required vendored libraries; desktop/GL loop code is
excluded from the XDK project. GitHub has not reported a smoke-workflow run yet,
and this workflow does not validate the XDK target.

### 3. Port the filesystem first

**Adapter implementation committed; not XDK-tested.** The XDK filesystem now
fills all 20 modern vtable entries: path resolution, text/binary file access,
rename/delete, streaming binary handles, directory operations and directory
enumeration. Reads/writes check return values and exact byte counts. Runtime
validation is still required for XDK-specific filesystem behavior.

### 4. Port audio

**Partial adapter implementation committed; not XDK-tested.** XAudio2 now binds
the modern 31-entry audio vtable, retains WAV/OGG playback, pitch, looping,
pause/resume and gain, and includes audio-group indexing plus lazy AUDO loading.
The stereo backend does not yet implement spatial routing, stream voices,
encoded playback or seeking; these callbacks return safe no-op/failure results
rather than claiming support. Audio-group and playback behavior still need
runtime testing.

### 5. Port rendering incrementally

**Pending separate renderer audit.** The old D3D9 renderer still implements the
legacy contract and is not yet compatible with modern `Runner_create`'s
renderer-vtable validation. This is deliberately not being patched in parallel
with the independent D3D9 audit, to avoid conflicting edits.

### 6. Reconcile the main loop and input

**Initial integration committed.** The XDK entry point now uses the modern
`Runner_create(dataWin, vm, renderer, fileSystem, audioSystem, seed)` signature
and the modern frame sequence (`Runner_drawPre`, `Runner_beginFrame`,
`Runner_drawViews`, draw-post and GUI passes). A platform swap helper now calls
`EndScene` and `Present` after the GUI pass, matching upstream's
`platformSwapBuffers()` placement. A duplicated `Gen8*` local declaration was
also removed. XInput-to-keyboard mappings are retained, with disconnected-
controller state cleared. WAD 17 TGIN parsing is enabled. Timing and
room-transition behavior still require runtime testing.

### 7. Build and test on the actual toolchain

A successful source review is not a successful Xbox build. Compilation and
runtime validation require the compatible Xbox 360 XDK/MSVC environment (or a
documented compatible setup). Do not mark a stage complete until it has been
built and tested.

## Current status

- [x] Confirm repository write access and create the isolated development branch.
- [x] Inspect modern renderer/audio/filesystem contracts and major API drift.
- [x] Import the selected modern core/source/dependency snapshot.
- [x] Implement the expanded XDK filesystem vtable.
- [x] Adapt XAudio2 to the modern audio vtable (with documented unsupported features).
- [x] Import modern CMake/CLI sources and configure a headless core smoke workflow.
- [x] Reconcile the XDK project source lists and modern runner startup/frame lifecycle.
- [ ] Confirm the headless GitHub Actions smoke build passes.
- [ ] Complete modern D3D9 renderer vtable implementation (separate audit in progress).
- [ ] Compile the XDK project with the compatible Xbox 360 toolchain.
- [ ] Run Undertale and WAD 17-era Deltarune test cases on hardware/emulator.

## Important build caveat

No Xbox 360 XDK compiler is available in this work session, so the XDK changes
have not been compiled or run. The CI workflow is configured but no run result
has been observed. The modern renderer contract still blocks `Runner_create`
until D3D9 satisfies the required vtable entries.
