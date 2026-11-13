# Butterscotch 360 Refresh 2 — Migration Audit

Status: initial migration audit; no runtime code has been ported yet.

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

Bring the current upstream core into the working branch while preserving the
Xbox 360 platform directory and XDK project files as a separate integration
layer. Keep the original `main` branch untouched.

### 2. Reconcile platform/build configuration

Compare upstream CMake/platform selection and source lists with the XDK Visual
Studio project. Identify every source file required by the modern core and
every desktop-only dependency that must be excluded or replaced for Xbox 360.

### 3. Port the filesystem first

Implement the modern `FileSystem` contract in the XDK backend. Verify path
normalization, short reads, exact reads, writes, directory enumeration and
streaming behavior. Check every XDK file API return value and byte count.

### 4. Port audio

Adapt XAudio2 to the modern `AudioSystem` contract. Keep decoding and playback
separate, then validate pitch, looping, pause/resume, streaming, group gain and
cleanup behavior.

### 5. Port rendering incrementally

Adapt D3D9 to the modern renderer contract in small groups: frame/view lifecycle,
2D primitives and sprites, render targets/surfaces, blend state, shaders, and
3D/vertex operations. Unsupported capabilities must be explicit and safe;
they must not leave uninitialized vtable entries.

### 6. Reconcile the main loop and input

Use the modern runner lifecycle and input expectations. Retain XInput support,
but avoid carrying over old assumptions about frame pacing, room drawing and
audio update order without validating them against upstream.

### 7. Build and test on the actual toolchain

A successful source review is not a successful Xbox build. Compilation and
runtime validation require the compatible Xbox 360 XDK/MSVC environment (or a
documented compatible setup). Do not mark a stage complete until it has been
built and tested.

## Current status

- [x] Confirm repository write access.
- [x] Confirm working branch exists and is based on the old Xbox port's `main`.
- [x] Inspect modern renderer/audio/filesystem contracts.
- [x] Identify major runtime and data-format drift.
- [ ] Import/reconcile modern upstream core.
- [ ] Port XDK filesystem.
- [ ] Port XAudio2 backend.
- [ ] Port D3D9 renderer.
- [ ] Reconcile XDK build project and main loop.
- [ ] Build and test with XDK.
