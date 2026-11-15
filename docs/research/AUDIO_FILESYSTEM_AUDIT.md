# Audio and Filesystem Audit — Xbox 360 migration

**Audit date:** 2026-11-13  
**Scope:** Static source audit only. No XDK build, console execution, audio playback test, or filesystem integration test was performed.  
**Renderer:** Excluded.

## Revisions inspected

| Repository | Ref inspected | Commit |
|---|---|---|
| Modern upstream `ButterscotchRunner/Butterscotch` | `main` | `0c94bfb05e3cb4b8946a38f8085747035dd37f94` |
| Original Xbox 360 port `ceilingtilefan/Butterscotch-360` | `main` | `1fd7659a4b95e0ce9837c3162de976cfc9ceec5a` |
| This fork `Viniqpp/Butterscotch-360-2` | `refresh2/modern-upstream-migration` | `5ac4091448877fc1ce738f69d77b0c64df956925` |

The fork branch advanced from `39fc2d4b9c5163f7d1a931a743e8767991974a6c` to the inspected head while this audit was in progress. The intervening two commits added only `docs/research/GAME_COMPATIBILITY.md` and `docs/research/XDK_BUILD_ENVIRONMENT.md`; the audio, filesystem, interface, and project files inspected below have identical blob SHAs at the final head.

The active Visual Studio Xbox 360 project compiles `src/xbox360-xdk/xaudio2_audio.cpp` and `src/xbox360-xdk/xdk_file_system.cpp` (`Butterscotch360.vcxproj:122-126`). Findings about the active port therefore refer to those XDK implementations, not the separate legacy `src/xbox360/` backend.

## Executive summary

- The current interfaces contain **31 AudioSystem slots** and **20 FileSystem slots**. The active XDK backend assigns every slot, so there is no uninitialized vtable entry in the selected XDK build.
- The XDK filesystem is substantially implemented: read/write modes, binary handles, seek/tell/size/rewrite, directory operations, and enumeration are present. Its ordinary read/write semantics are mostly aligned with the interface.
- The XDK audio backend is only a partial implementation of the modern contract. The most serious confirmed issue is unsafe WAV chunk parsing that can read beyond the input buffer. External-file and decoder allocation failures can also crash.
- Audio group failure is reported as success, timed group-gain fades never advance, and several modern audio operations are stubs or incomplete.
- No compilation or runtime behavior is claimed.

## Post-audit implementation notes

The working branch has since received source-level hardening changes:
- `xaudio2_audio.cpp`: checked external-file reads and allocation failures; bounded RIFF/WAVE chunk parsing and PCM copies; rejects unsupported/truncated WAV/OGG data; makes failed audio-group loads retryable and only reports successfully parsed groups as loaded.
- `xdk_file_system.cpp`: checks `SetFilePointer` and `SetEndOfFile` results in binary rewrite.
- `main.cpp`: validates optional CONFIG.JSN size, allocation, and complete read before parsing.
- `common.h`: recognizes the Xbox 360/Xenon target as big-endian through the XDK project's `_XBOX` define (also `_M_PPC`).

These are static source changes only. No XDK compilation, console execution, or emulator validation has been performed. Re-audit the latest branch contents before treating the findings as closed.

## Confirmed issues

### A-01 — WAV chunk parsing can read beyond the input buffer

**Severity: High — potential crash / memory disclosure or corruption**  
**Location:** `src/xbox360-xdk/xaudio2_audio.cpp`, `xdkPlaySound()`, approximately lines 286-326.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp#L286-L326)

The WAV path reads fixed offsets for channels, sample rate, and bit depth, then walks RIFF chunks using a file-controlled `chunkSize`. The loop checks that an 8-byte chunk header is present, but it does not validate that the declared chunk payload fits inside `oggSize`. Once it finds a `data` chunk, it copies `dataSize` bytes from `oggData + dataOffset` without checking that the source range is within the buffer. A malformed or truncated WAV can therefore cause an out-of-bounds read in `memcpy`; signed chunk-size arithmetic can also overflow or move the offset outside the file.

The parser also assumes a conventional fixed WAV header layout instead of validating the RIFF/WAVE and `fmt ` chunks, so valid WAVs with unusual chunk ordering or extended format metadata may be rejected or misread.

**Recommended correction:** Parse RIFF chunks with checked unsigned offsets. Validate the RIFF/WAVE signature, every chunk header and payload, `fmt ` size/format tag, channel count, sample rate, block alignment, bit depth, and data range. Before copying, require `dataOffset <= oggSize` and `dataSize <= oggSize - dataOffset`. Reject unsupported encodings cleanly.

**Suggested test:** Feed truncated headers, a chunk size larger than the remaining file, a missing `data` chunk, a chunk-size overflow, odd-sized padded chunks, reordered `JUNK`/`fmt `/`data chunks, and a valid PCM WAV. Run under a memory checker or with guard pages where possible.

### A-02 — Unchecked external audio and PCM allocations can crash

**Severity: High — potential crash / failed or corrupted audio playback**  
**Location:** `src/xbox360-xdk/xaudio2_audio.cpp`, `xdkPlaySound()`, external-file read and OGG/WAV decode branches, approximately lines 260-350.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp#L192-L350)

The external-file path assigns `malloc(oggSize)` to `oggData` and immediately passes it to `fread` without checking allocation success. It also ignores the number of bytes returned by `fread`. The WAV path similarly does not check `malloc(dataSize)` before `memcpy`. The OGG path trusts `stb_vorbis_stream_length_in_samples()` and multiplies `totalSamples * channels * sizeof(short)` without validating positive dimensions, overflow, or allocation success before decoding into the result.

These are reachable failure cases on a memory-constrained console, and malformed/short files can also produce incomplete input.

**Recommended correction:** Check `fseek`/`ftell`, reject sizes that cannot be represented safely, verify allocation results, and require `fread` to return the full requested byte count. Validate OGG channel count, sample rate, total sample count, multiplication bounds, and decoder output before allocating or submitting a voice. On every failure, release temporary buffers and retire the instance immediately.

**Suggested test:** Force allocation failures at each allocation site; use truncated external OGG/WAV files; test zero-length and very large files; verify no crash, leaked instance slot, or partially initialized voice remains.

### A-03 — Failed audio-group loads are reported as loaded and cannot be retried

**Severity: High — missing assets / misleading API result**  
**Location:** `src/xbox360-xdk/xaudio2_audio.cpp`, `xdkGroupLoad()` and `xdkGroupIsLoaded()`, lines 589-639.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp#L589-L639)

When the group file is missing, path resolution fails, or `DataWin_parse()` fails, `xdkGroupLoad()` appends a zeroed placeholder `DataWin` to `audioGroups`. `xdkGroupIsLoaded()` only checks whether the array length exceeds the requested index. It therefore returns true for a placeholder that contains no loaded group data. Because the placeholder occupies the index, a later call to `xdkGroupLoad()` does not retry the load.

This makes transient storage errors and missing group files look like successful loads. A later sound lookup sees an empty `audo` table and cannot play the asset.

**Recommended correction:** Track group state explicitly (unrequested/loading/loaded/failed), or store a nullable group pointer plus a separate state array. Return loaded only when parsing succeeded. Permit retry after failure, and log the path plus parse error. Preserve index alignment without using a valid-looking empty `DataWin` as the success signal.

**Suggested test:** Request a missing group, assert `groupIsLoaded == false`, then create the file and request it again; assert the second request succeeds and embedded audio plays. Also test malformed group data and sparse group indices.

### A-04 — Timed audio-group gain fades

**Status: Withdrawn — the finding is not reproducible in the audited source.**

The inspected XDK implementation already calls `AudioSystem_updateGroupGains(audio, deltaTime)` from `xdkAudioUpdate()` and reapplies the effective group gain when it changes. The original claim that the update call was missing was incorrect. This behavior still needs runtime verification on XDK, but it is not a confirmed source defect.

### A-05 — SOND pitch is ignored; asset-index pitch/gain accessors do not match upstream semantics

**Severity: Medium — incorrect sound pitch and GML control behavior**  
**Location:** `xdkPlaySound()` lines 192-250, `xdkSetSoundGain()`/`xdkGetSoundGain()` lines 500-536, and `xdkSetSoundPitch()`/`xdkGetSoundPitch()` lines 538-556.  
[Playback source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp#L192-L250) · [Controls source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp#L500-L556)

The instance initializes `sondPitch` to 1.0 and never copies `sound->pitch`, while upstream initializes playback from the SOND pitch value. In addition, the XDK pitch setter/getter only handle active instance IDs; calls with a SOND index silently do nothing / return 1.0. The gain setter finds only the first active instance for a SOND index, whereas upstream updates every active instance with that sound index. The gain getter returns 0.0 for a SOND index rather than reporting an active instance's gain.

**Recommended correction:** Initialize `sondPitch` from the sound resource's pitch value and apply it to the voice. Match upstream's ID discrimination and sound-index behavior for pitch and gain; update all matching instances when the argument is a sound index. Keep getters consistent with the documented GameMaker-facing semantics.

**Suggested test:** Play two instances of one sound, set pitch and gain by SOND index, and verify both voices change. Verify getters by SOND index and by instance ID. Include a sound resource whose stored pitch is not 1.0.

### A-06 — Sound length is only available while an instance is active

**Severity: Medium — incorrect asset metadata result**  
**Location:** `src/xbox360-xdk/xaudio2_audio.cpp`, `xdkGetSoundLength()`, lines 664-681.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp#L664-L681)

The function searches active instances and returns zero if none is found. The modern backend can determine the duration from the embedded decoder or external file even when the sound has not been played. As a result, a GameMaker length query made before playback returns 0 for an otherwise valid sound.

**Recommended correction:** Resolve the SOND resource and determine duration from its embedded AUDO data or external file when no active instance exists. Cache the result if repeated decoder initialization is too expensive.

**Suggested test:** Query the length of embedded and external sounds before playback, during playback, after playback ends, and for invalid indices.

### A-07 — Track seeking is a no-op, and loop position is not wrapped

**Severity: Medium — broken audio timeline controls**  
**Location:** `xdkSetTrackPosition()` lines 571-574 and `xdkGetTrackPosition()` lines 558-569.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp#L558-L574)

`xdkSetTrackPosition()` explicitly does nothing. Also, `xdkGetTrackPosition()` returns `SamplesPlayed / sampleRate` directly. For looping voices, this is cumulative playback time rather than the current position within the loop; upstream wraps the cursor by the known track length.

**Recommended correction:** Retain decoded PCM length and implement seeking by stopping/flushing and resubmitting the buffer from a validated frame offset, preserving loop state. Return the cursor modulo the loop length for looping sounds. Reject negative and non-finite seek positions.

**Suggested test:** Seek to zero, midpoint, exact end, and beyond end; verify the cursor and audible output. Play a looping sound for several loops and verify reported position remains within one track duration.

### A-08 — Spatial positioning and listener-specific gain are not implemented

**Severity: Medium — missing audio behavior**  
**Location:** `xdkSetSoundSpatial()` lines 642-648 and `xdkSetMasterGainForListener()` lines 682-685.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp#L642-L685)

`xdkSetSoundSpatial()` discards all position, velocity, distance, and rolloff parameters. `xdkSetMasterGainForListener()` ignores the listener ID and applies global master gain. The modern backend applies spatialization and maintains per-listener gain groups. The XDK backend is currently a stereo-only path, so this is a known feature gap rather than an accidental null vtable slot.

**Recommended correction:** Implement X3DAudio/XAudio2 spatial routing and listener-specific submix/voice groups, or explicitly document unsupported behavior and ensure callers do not assume the parameters took effect.

**Suggested test:** Move a sound source around a fixed listener and verify pan/attenuation; use two listener IDs with different gains and verify isolation.

### A-09 — Audio stream and encoded-playback APIs are stubs

**Severity: Medium — unsupported modern audio paths**  
**Location:** `xdkCreateStream()`, `xdkDestroyStream()`, and `xdkPlayEncoded()`, lines 697-710.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp#L697-L710)

Stream creation always returns -1, stream destruction always returns false, and encoded playback always returns -1. Upstream implements stream entries and decoder-backed encoded voices. These slots are populated, but their functionality is not available on XDK. Any game or extension using these paths will fail to start audio.

**Recommended correction:** Implement stream entries with owned file/decoder/voice state and deterministic teardown. For encoded playback, decode from the supplied memory and retain any required source bytes or decoder state until the voice ends or is stopped, as required by the interface contract. Until then, document the limitation at the platform capability boundary.

**Suggested test:** Create/play/destroy a streamed OGG; test invalid and repeated stream destruction; play encoded memory with loop on/off; stop during playback; destroy the audio system while voices remain active.

### A-10 — Vtable initialization is not synchronized

**Severity: Low / possible risk — race during concurrent initialization**  
**Location:** `xdkInitializeAudioVtable()` lines 715-751 and `XdkFileSystem_create()` lines 331-359.  
[Audio source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp#L715-L751) · [Filesystem source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xdk_file_system.cpp#L331-L359)

Both modules lazily populate a process-global vtable behind a plain boolean. Concurrent first-time creation can race while the table is being written or read. The normal startup path may be single-threaded, so this is a risk, not a demonstrated failure.

**Recommended correction:** Use static compile-time initialization where possible, or a platform-supported one-time initialization primitive. Do not expose the table until all entries are assigned.

**Suggested test:** Repeatedly create audio/filesystem instances from multiple threads on a host test build with race detection, then invoke every vtable entry.

### F-01 — Binary rewrite ignores truncation errors

**Severity: Medium / possible risk — save file may remain untruncated**  
**Location:** `src/xbox360-xdk/xdk_file_system.cpp`, `xdkBinaryRewrite()`, lines 263-267.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xdk_file_system.cpp#L263-L267)

The function calls `SetFilePointer()` and `SetEndOfFile()` but ignores both return values. If the seek fails, the truncate can be attempted at the previous cursor position; if truncation fails, the caller receives no indication. Since the interface returns void, the backend should at least log failures and avoid attempting truncation after a failed seek.

**Recommended correction:** Check the seek result (with the Win32 `SetLastError(NO_ERROR)` convention when interpreting `INVALID_SET_FILE_POINTER`), then check `SetEndOfFile()`. Log failures with the handle/path if available. Consider revisiting the interface if callers need reliable failure reporting.

**Suggested test:** Open an existing file, position the cursor at a nonzero offset, rewrite, and verify size is exactly zero and cursor is at zero. Inject invalid-handle and seek/truncate failures and verify they do not silently report success.

## Possible risks / follow-up checks

### R-01 — Path prefix normalization is incomplete

**Severity: Low / possible risk**  
**Location:** `xdkResolvePath()`, lines 10-30; `XdkFileSystem_create()`, lines 361-375.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xdk_file_system.cpp#L10-L30)

The resolver converts forward slashes only in the appended relative-path portion (`result + baseLen`), not in the base path copied from `dataWinPath`. The constructor accepts either slash style when locating the directory separator, so a forward-slash input can leave a mixed-separator path. It also silently truncates a base path longer than the fixed 512-byte buffer. Verify actual XDK path parsing for mixed separators; normalize the full result and reject overlong base paths rather than silently truncating.

**Suggested test:** Construct the filesystem with both `game:\\folder\\data.win` and `game:/folder/data.win`, then resolve/read a sibling asset. Test a base path at and beyond the 511-byte limit.

### R-02 — Filesystem operations are not serialized

**Severity: Low / possible risk**  
**Location:** All `xdkBinary*` functions and the shared XDK filesystem implementation.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xdk_file_system.cpp)

The backend returns raw OS handles and does not add locks. Independent handles are naturally separate, but simultaneous operations on the same handle are not coordinated. This is acceptable if the VM guarantees per-handle single-thread ownership; that guarantee was not verified in this audit.

**Recommended correction:** Document handle thread affinity/ownership, or serialize operations if scripts, async tasks, or worker threads can share a binary handle.

**Suggested test:** Exercise independent handles concurrently, then deliberately share one handle across threads and verify the runtime either prevents it or serializes it.

### R-03 — Legacy libxenon backend is not compatible with the expanded interfaces

**Severity: High if selected; not active in the inspected Visual Studio XDK build**  
**Location:** `src/xbox360/xbox360_audio_system.c`, `x360AudioVtable` lines 448-470; `src/xbox360/xbox360_file_system.c`, `x360FileSystemVtable` lines 73-81.  
[Audio source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360/xbox360_audio_system.c#L448-L470) · [Filesystem source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360/xbox360_file_system.c#L73-L81)

The legacy backend assigns only a subset of the current vtables: its audio initializer leaves modern entries unset, and its filesystem initializer assigns only the original five operations. Calling a newly added entry through either table would dereference a null function pointer. Additionally, `x360PlaySound()` creates silent instances and `decodeSound()` currently always returns null. The inspected `Butterscotch360.vcxproj` compiles the XDK backend instead, so this is not an active-build finding; it is a warning against selecting the legacy backend without completing its port.

**Recommended correction:** Either remove the obsolete backend from supported build configurations or implement every interface entry with an explicit supported/no-op/failure behavior. Never leave callable vtable entries null.

**Suggested test:** Build each supported backend configuration and run a vtable completeness test that checks every required function pointer before engine startup.

## Filesystem contract review

The active `FileSystemVtable` has 20 entries (`src/file_system.h`). The XDK constructor assigns all 20 in `XdkFileSystem_create()` (lines 337-359). The following behavior was verified by source inspection:

| Area | XDK behavior | Assessment |
|---|---|---|
| Path resolution | Relative paths are rooted beside `data.win`; drive/absolute paths pass through; forward slashes in the relative suffix become backslashes | Mostly aligned; see R-01 |
| Case handling | Delegates to XDK filesystem APIs; no explicit case-folding | Expected to rely on Xbox filesystem behavior; verify mixed-case asset paths on target media |
| Text reads/writes | Exact-read loop for text; writes use `CREATE_ALWAYS` and check full write | Correct basic semantics; large-file allocation limits remain |
| Whole-file binary read/write | Initializes output pointers, handles empty files, checks allocation and full reads/writes; rejects sizes above signed 32-bit range | Good |
| Binary open modes | READ uses `OPEN_EXISTING`; WRITE uses `CREATE_ALWAYS`; READWRITE uses `OPEN_ALWAYS` and preserves existing bytes | Matches declared modes |
| Binary read/write | Returns byte count, 0 at zero-length request/EOF, -1 on OS error | Reasonable; callers must handle short I/O |
| Tell/seek/size | Uses file pointer and signed 32-bit size API | Mostly correct within the 2 GiB API range |
| Rewrite | Seeks to zero and truncates, but ignores errors | See F-01 |
| Rename/delete/directories | Uses MoveFile/DeleteFile/CreateDirectory/RemoveDirectory | Basic behavior implemented; rename-to-existing and non-recursive directory creation follow OS semantics |
| Directory enumeration | Excludes `.` and `..`; returns allocated names in an stb_ds array | Matches ownership contract; caller must free each name and `arrfree()` the array |

No confirmed binary-open mode inversion or seek-origin bug was found. `GML_FILE_BIN_READWRITE` preserves existing contents and starts at the beginning; `GML_FILE_BIN_WRITE` truncates/creates; binary read/write report the actual byte count. The main filesystem concerns are rewrite error handling, path normalization/truncation, and unverified same-handle concurrency.

## Audio ownership, WAD, and lifecycle review

- **Main DataWin ownership:** `xdkAudioInit()` stores the caller-owned main `DataWin` at audio-group index 0. `xdkAudioDestroy()` frees groups from index 1 onward, then releases the array; it does not free the caller-owned main data file.
- **Loaded group ownership:** Parsed nonzero groups are owned by the audio system and freed during destroy. The zeroed placeholders created on load failure are also passed to `DataWin_free()`; the inspected implementation of `DataWin_free()` frees zero-initialized members safely.
- **Embedded audio lifetime:** `DataWin_loadAudoIfNeeded()` populates the selected AUDO entry. The XDK backend decodes to a separate PCM allocation and keeps that PCM buffer in the instance until the voice is stopped/destroyed or playback finishes. This is the right general lifetime model for XAudio2's submitted buffer.
- **Voice cleanup:** `destroyInstance()` stops and flushes the source voice, destroys it, then frees PCM. This ordering is appropriate. `xdkAudioDestroy()` tears down instances before the mastering voice and XAudio2 engine.
- **WAD classification:** `xdkPlaySound()` uses the modern regular/embedded/compressed flag test to choose external files versus AUDO data. It calls `DataWin_loadAudoIfNeeded()` for embedded entries. The classification broadly matches upstream; the group-load failure state and parser robustness are the problems noted above.
- **Threading:** XAudio2 manages its own audio thread, but the backend's instance table, group array, and gain state are mutated without locks. If the engine calls these methods only from its main/game thread, this may be intentional; cross-thread calls were not proven safe.

## Recommended test plan before calling the port audio/filesystem complete

1. **Vtable completeness:** Assert all 31 audio and 20 filesystem pointers are non-null for the XDK backend.
2. **Audio decode:** Play embedded OGG, external OGG, valid PCM WAV, truncated WAV, malformed RIFF, and missing external file. Confirm no crashes and that failed sounds do not remain active.
3. **Audio groups:** Load group 0 and a nonzero group; test missing, malformed, and subsequently restored group files; verify `groupIsLoaded` and retry behavior.
4. **Audio controls:** Test resource pitch, instance pitch, sound-index pitch/gain affecting multiple instances, timed sound gain, timed group gain, master gain, and listener gain.
5. **Timeline:** Test sound length before/after play, seek, looping track position, pause/resume, and stop during playback.
6. **Streaming:** Test stream and encoded playback or explicitly mark those APIs unsupported at the engine/platform boundary.
7. **Binary I/O:** Round-trip binary files in all three modes; test EOF, short read/write, negative arguments, seek bounds, tell, size, rewrite/truncate, and zero-byte files.
8. **Paths and directories:** Test drive-absolute and relative paths, both separator styles, case variation, nested directory creation, enumeration ownership, rename/delete, and save paths on the actual target filesystem.
9. **Failure injection:** Force allocation, file-open, parse, and XAudio2 voice-creation failures; verify no leaks, stale handles, active-but-silent instances, or corrupted saves.

## Source references

- [Modern upstream audio interface](https://github.com/ButterscotchRunner/Butterscotch/blob/0c94bfb05e3cb4b8946a38f8085747035dd37f94/src/audio_system.h)
- [Modern upstream filesystem interface](https://github.com/ButterscotchRunner/Butterscotch/blob/0c94bfb05e3cb4b8946a38f8085747035dd37f94/src/file_system.h)
- [Modern upstream miniaudio implementation](https://github.com/ButterscotchRunner/Butterscotch/blob/0c94bfb05e3cb4b8946a38f8085747035dd37f94/src/audio/miniaudio/ma_audio_system.c)
- [Modern upstream overlay filesystem](https://github.com/ButterscotchRunner/Butterscotch/blob/0c94bfb05e3cb4b8946a38f8085747035dd37f94/src/overlay_file_system.c)
- [Original Xbox 360 XDK audio implementation](https://github.com/ceilingtilefan/Butterscotch-360/blob/1fd7659a4b95e0ce9837c3162de976cfc9ceec5a/src/xbox360-xdk/xaudio2_audio.cpp)
- [Original Xbox 360 XDK filesystem implementation](https://github.com/ceilingtilefan/Butterscotch-360/blob/1fd7659a4b95e0ce9837c3162de976cfc9ceec5a/src/xbox360-xdk/xdk_file_system.cpp)
- [Fork XDK audio implementation](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp)
- [Fork XDK filesystem implementation](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xdk_file_system.cpp)
- [Fork project build source list](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/Butterscotch360.vcxproj)


---

# SECOND RESEARCH PASS — Deeper audio and filesystem revalidation

**Research date:** 2026-11-13  
**Scope:** AudioSystem and FileSystem only; renderer findings remain in the separate renderer audit.  
**Method:** Static source and Git-object inspection only. No XDK compilation, XEX generation, emulator/console run, audio playback, filesystem integration test, or fault injection was performed.

## Exact revisions inspected

| Repository | Ref / commit |
|---|---|
| Fork | `Viniqpp/Butterscotch-360-2`, `refresh2/modern-upstream-migration`, **`ddddd4ebb1e65afdc4c675f3d6d628a3e2dae971`** |
| Fork source snapshot parent | **`d61018a1e326e1b0034f9dd3879677aa36523abe`** |
| Modern upstream | `ButterscotchRunner/Butterscotch`, `main`, **`0c94bfb05e3cb4b8946a38f8085747035dd37f94`** |
| Original port reference | `ceilingtilefan/Butterscotch-360`, **`1fd7659a4b95e0ce9837c3162de976cfc9ceec5a`** |

The current branch HEAD `ddddd4e...` changes only `docs/research/INDEPENDENT_CODE_REVIEW.md`; its parent `d61018a...` changes only `docs/research/XDK_BUILD_ENVIRONMENT.md`. The source snapshots below are therefore unchanged at the inspected HEAD. The pre-edit report blob was `1acc0006e1117cd7204fbabf52a83c0435af5d09`; that full report was fetched and preserved as the base of this append.

### Fork source blob SHAs

| Path | Blob SHA |
|---|---|
| `src/xbox360-xdk/xaudio2_audio.cpp` | `84bb2863b60b37507f9898c1597115f1ac13da09` |
| `src/xbox360-xdk/xdk_file_system.cpp` | `04d03335ca8bec185b89c6ce5d97c7c0341685c3` |
| `src/audio_system.h` | `f4e5e998dd558d81493b76c9c2cbaa268da7ea08` |
| `src/file_system.h` | `b4e8c28f09cf9a8ab10565778c105ed83d300ec2` |
| `src/data_win.c` | `26d1533e8275b8809f83f425be0919496c161d21` |
| `Butterscotch360.vcxproj` | `2fc2bb5bc081339d02d0156f8829310d0fdfed7d` |

Upstream interface headers are byte-identical to the fork headers at these snapshots. Upstream implementation blobs: `src/audio/miniaudio/ma_audio_system.c` = `63d53f803cda96b9af27ec2229dfb8a185213c90`; `src/overlay_file_system.c` = `0e3d9aed63964549fe406c6f73326b466e27381a`.

## 1. Revalidation of first-pass findings

| First-pass finding | Current status | Second-pass conclusion |
|---|---|---|
| A-01 WAV RIFF chunk out-of-bounds read | **Original defect fixed; residual hardening gap** | Current parser adds RIFF/chunk/data bounds checks. Malformed chunk handling should still fail closed instead of silently breaking and accepting partial parse state. |
| A-02 unchecked external reads and PCM allocations | **Fixed** | Current source checks external open/seek/size/read and WAV/OGG allocations, including short external reads. |
| A-03 failed groups reported loaded | **Fixed** | Sparse group slots use NULL; `groupIsLoaded` requires a non-NULL parsed DataWin; failed loads remain retryable. |
| A-04 group fades never advance | **Fixed** | `xdkAudioUpdate` now advances group gain state and reapplies volume to active voices. |
| A-05 SOND pitch and sound-index controls | **Partially fixed** | Initial SOND pitch is copied and setters update all active matching instances. XAudio2 `SetFrequencyRatio` return/range handling remains weak. |
| A-06 sound length only while active | **Confirmed** | `xdkGetSoundLength` returns 0 for a valid SOND with no active instance. |
| A-07 seek no-op / loop position | **Confirmed** | `xdkSetTrackPosition` remains a TODO; getter is instance-ID-only and reports cumulative SamplesPlayed, not loop-relative cursor. |
| A-08 spatial audio/listener gain ignored | **Confirmed** | Spatial args are discarded; listener gain is collapsed to global master gain. Listener position is stored but not used for spatial routing. |
| A-09 streams/encoded playback stubs | **Confirmed** | Stream creation/destruction and encoded playback still fail unconditionally. |
| A-10 lazy vtable initialization race | **Hypothesis** | Plain static booleans guard lazy vtable writes; concurrent first construction is not established by source alone. |
| F-01 rewrite ignores failures | **Fixed** | Current code checks seek/truncate failures and logs them; the void interface still cannot report structured failure to callers. |
| R-01 path base normalization/truncation | **Confirmed** | Only relative suffix separators are normalized; base path can remain mixed; fixed 512-byte path storage silently truncates. |
| R-02 same-handle concurrency | **Hypothesis / contract gap** | No serialization exists; impact depends on VM handle-threading guarantees. |
| R-03 legacy libxenon vtables incomplete | **Confirmed, not active in current XDK project** | Legacy backend remains incomplete, but the Visual Studio project selects XDK backend. |

### A-01 follow-up — malformed WAV chunks should fail closed

**Classification:** Confirmed hardening gap; the original out-of-bounds-copy defect is fixed.  
**Location:** `src/xbox360-xdk/xaudio2_audio.cpp`, `xdkPlaySound()`, RIFF parser.

The parser now has bounds checks, but invalid chunk headers/payloads or undersized `fmt ` chunks should be distinguished from normal end-of-list. Do not continue with previously discovered format/data values after a malformed chunk.

**Correction:** Return an explicit parse status and validated PCM view. Reject malformed chunk structure, invalid padding, inconsistent RIFF/input sizes, unsupported encodings and conflicting format metadata. Retain subtraction-based bounds checks.

**Tests:** Valid PCM with odd-sized padded ancillary chunks; data-before-fmt; truncated header; undersized fmt; payload beyond RIFF; RIFF size beyond input; malformed trailing chunk after valid chunks.

### A-05 follow-up — pitch application needs validation and HRESULT handling

**Classification:** Confirmed missing error handling; exact audible impact requires target testing.  
**Location:** `xdkPlaySound()`, `xdkSetSoundPitch()`, `src/xbox360-xdk/xaudio2_audio.cpp`.

The adapter calls `SetFrequencyRatio` but does not check the HRESULT or validate finite positive values against the actual supported XAudio2 ratio range. Stored pitch can therefore diverge from the effective voice setting.

**Correction:** Validate finite positive pitch, clamp/reject according to the targeted XDK's voice limits, check HRESULT, and keep stored/effective pitch consistent.

**Tests:** Default, lower/upper supported bounds, out-of-range, zero, negative, NaN and infinity; compare reported pitch with effective voice ratio.

## 2. New confirmed audio findings

### A-11 — successful playback destroys its voice before returning

**Severity:** Critical — normal playback cannot remain active.  
**Location:** `src/xbox360-xdk/xaudio2_audio.cpp`, `xdkPlaySound()`, immediately after successful `IXAudio2SourceVoice::Start()`, approximately lines 480–495.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/ddddd4ebb1e65afdc4c675f3d6d628a3e2dae971/src/xbox360-xdk/xaudio2_audio.cpp#L470-L500)

After decode, voice creation, buffer submission and Start all succeed, the function unconditionally calls `destroyInstance(inst)` and returns -1. That helper stops/flushes/destroys the voice, frees PCM and marks the slot inactive. The successful voice is therefore torn down before the caller can use an instance ID.

**Correction:** Remove success-path destruction and return `inst->instanceId`. Keep cleanup on failure and natural completion/explicit stop. PCM remains instance-owned until the voice has stopped and been destroyed.

**Tests:** Play embedded OGG and PCM WAV; require nonnegative ID, active state after return and queued buffer retention. Verify PCM survives until completion/stop and the slot is reclaimed afterward.

### A-12 — instance IDs are derived from reusable slot indices

**Severity:** High — stale handles can control an unrelated later sound.  
**Location:** `findFreeSlot()`, `findById()`, instance initialization in `xdkPlaySound()`, `src/xbox360-xdk/xaudio2_audio.cpp`.

IDs are computed from `XDK_SOUND_INSTANCE_ID_BASE + slotIndex`; `nextInstanceCounter` does not participate. Once a slot is freed and reused, its ID is reused too. Old script-held IDs may stop, pause or modify a new sound in that slot.

**Correction:** Assign monotonic IDs from the counter, store each ID in its slot, and compare it in lookup. Define wraparound while skipping reserved ranges and active IDs; reserve a disjoint range for future stream IDs.

**Tests:** Play/stop a sound, reuse its slot, then issue stop/pause/gain using the old ID. The new voice must be unaffected. Add reduced-width counter wraparound tests.

### A-13 — valid sound length cannot be queried before playback

**Severity:** Medium — incorrect GameMaker-facing metadata.  
**Location:** `xdkGetSoundLength()`, `src/xbox360-xdk/xaudio2_audio.cpp`, approximately lines 787–803.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/ddddd4ebb1e65afdc4c675f3d6d628a3e2dae971/src/xbox360-xdk/xaudio2_audio.cpp#L780-L810) · [Upstream](https://github.com/ButterscotchRunner/Butterscotch/blob/0c94bfb05e3cb4b8946a38f8085747035dd37f94/src/audio/miniaudio/ma_audio_system.c)

Duration is derived only from an active instance's PCM. A valid SOND with no active instance returns 0, unlike upstream's temporary decoder query.

**Correction:** Resolve SOND/group/AUDO or external source, open a temporary decoder, query frames/sample rate, and close/free on every path. Cache by immutable asset identity if needed.

**Tests:** Query embedded/external duration before play, during play, after natural completion and after stop; invalid indices return 0.

### A-14 — track-position getter omits SOND lookup and loop-relative cursor

**Severity:** Medium — incomplete API parity.  
**Location:** `xdkGetTrackPosition()`, `src/xbox360-xdk/xaudio2_audio.cpp`, approximately lines 675–693.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/ddddd4ebb1e65afdc4c675f3d6d628a3e2dae971/src/xbox360-xdk/xaudio2_audio.cpp#L660-L700) · [Upstream](https://github.com/ButterscotchRunner/Butterscotch/blob/0c94bfb05e3cb4b8946a38f8085747035dd37f94/src/audio/miniaudio/ma_audio_system.c)

The getter only resolves instance IDs. A SOND index returns 0 even with an active matching instance. Looping playback reports cumulative SamplesPlayed/sampleRate, not position within one iteration.

**Correction:** Match upstream ID-vs-SOND discrimination. Define elapsed-time vs track-cursor semantics; if cursor, normalize only looping instances using decoded frame count and loop behavior.

**Tests:** Query by ID and SOND; test non-looping and multiple loop iterations, inactive and invalid values.

### A-16 — DataWin parser can terminate the process on recoverable group-load errors

**Severity:** Critical — missing/unreadable/malformed group file can exit the game.  
**Location:** `xdkGroupLoad()`, `src/xbox360-xdk/xaudio2_audio.cpp`; `DataWin_parse()`, `src/data_win.c`, approximately lines 3056–3076.  
[Group loader](https://github.com/Viniqpp/Butterscotch-360-2/blob/ddddd4ebb1e65afdc4c675f3d6d628a3e2dae971/src/xbox360-xdk/xaudio2_audio.cpp#L720-L770) · [Parser](https://github.com/Viniqpp/Butterscotch-360-2/blob/ddddd4ebb1e65afdc4c675f3d6d628a3e2dae971/src/data_win.c#L3050-L3085)

The group loader checks existence and handles a NULL parse result, but `DataWin_parse()` calls `exit(1)` when `fopen` fails or file size is invalid. A race between existence check/open, unreadable storage, empty or corrupt group file can terminate the process. The caller's NULL check cannot catch process exit.

**Correction:** Make input-driven parse errors return NULL or structured errors after closing/freeing partial state. Audit all parser-reachable `exit(1)` calls. Preserve the group's NULL/unloaded state on failure so retry remains possible.

**Tests:** Delete file between exists/open; empty, truncated and malformed group files; process must survive, group remains unloaded, and a valid replacement loads successfully.

## 3. New confirmed filesystem findings

### F-02 — binary tell narrows an unsigned OS cursor to signed 32-bit

**Severity:** Medium — large valid cursor can become negative/wrapped.  
**Location:** `xdkBinaryTell()`, `src/xbox360-xdk/xdk_file_system.cpp`, approximately lines 240–246.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/ddddd4ebb1e65afdc4c675f3d6d628a3e2dae971/src/xbox360-xdk/xdk_file_system.cpp#L230-L255)

The OS cursor is returned as DWORD then cast to `int32_t`. Values above INT32_MAX are not representable and can wrap. The size helper already rejects sizes outside the signed interface range.

**Correction:** Return -1 when cursor exceeds INT32_MAX; widen the shared interface consistently if larger files are required.

**Tests:** Seek/tell around INT32_MAX using sparse files where supported; no wrapped negative cursor may be reported as valid.

### F-03 — text read can overflow size-plus-terminator allocation

**Severity:** Medium / low-likelihood robustness issue.  
**Location:** `xdkReadFileText()` and `xdkGetFileSize()`, `src/xbox360-xdk/xdk_file_system.cpp`, approximately lines 33–101.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/ddddd4ebb1e65afdc4c675f3d6d628a3e2dae971/src/xbox360-xdk/xdk_file_system.cpp#L25-L110)

The size helper rejects nonzero high DWORDs but accepts low DWORD 0xffffffff. `(size_t)size + 1` wraps on a 32-bit target, so the subsequent read can write into an undersized allocation.

**Correction:** Reject text sizes above the signed engine limit before adding the NUL byte; use checked addition and consistent bounds across whole-file APIs.

**Tests:** Unit-test size validation at 0, INT32_MAX, INT32_MAX+1 and UINT32_MAX without allocating those buffers.

### F-04 — directory enumeration silently returns partial results after allocation failure

**Severity:** Low/Medium — missing assets can look like a complete listing.  
**Location:** `xdkListDirectory()`, `src/xbox360-xdk/xdk_file_system.cpp`, approximately lines 294–328.  
[Source](https://github.com/Viniqpp/Butterscotch-360-2/blob/ddddd4ebb1e65afdc4c675f3d6d628a3e2dae971/src/xbox360-xdk/xdk_file_system.cpp#L285-L335)

If allocating one entry name fails, the implementation skips it and continues. The returned array gives no indication it is incomplete. It also does not inspect the final GetLastError after FindNextFileA stops.

**Correction:** On allocation failure, free all accumulated names, free the array, close the search handle and return NULL. Inspect terminal enumeration error rather than treating every stop as normal EOF.

**Tests:** Inject failure on Nth name allocation; require NULL and no leaked names/array/search handle. Test normal empty/populated directories.

## 4. Risks and unresolved contracts

### R-04 — lazy AUDO reads share a mutable FILE cursor and failed reads are not retryable

**Classification:** Missing error checks confirmed; concurrency impact remains a hypothesis.  
**Location:** `DataWin_loadAudoIfNeeded()`, `src/data_win.c`, approximately lines 3029–3050; called by `xdkPlaySound()`.

The helper does not check ftell/fseek results. A short read is logged but leaves a zero-initialized, non-NULL data pointer, so subsequent calls treat failed data as loaded. Concurrent loads can interleave seeks/reads on the same FILE cursor; whether normal engine calls can race depends on the threading contract.

**Correction:** Return success/failure; check all cursor/read calls; free partial buffer and leave entry->data NULL on failure. Enforce main-thread-only loading or protect the shared FILE cursor with a per-DataWin lock.

**Tests:** Inject seek/short-read failure and retry; concurrently load distinct AUDO entries and compare bytes to source.

### R-05 — external audio bypasses the FileSystem read API

**Classification:** Hypothesis / platform integration risk.  
**Location:** External-file branch of `xdkPlaySound()`, `src/xbox360-xdk/xaudio2_audio.cpp`.

The path is resolved through FileSystem but opened with CRT fopen, while the XDK filesystem uses CreateFileA. Source review does not prove CRT fopen accepts every device/mount path accepted by the XDK filesystem.

**Correction:** Prefer FileSystem.readFileBinary or add streaming-open/read to FileSystem. If CRT access is intentional, document and verify on target.

**Tests:** Play external OGG from game device and secondary mounted device; compare with FileSystem binary read.

### R-06 — vtable and audio state concurrency is not encoded

**Classification:** Hypothesis / concurrency risk.  
**Location:** `xdkInitializeAudioVtable()`, `XdkFileSystem_create()`, audio instance/group state.

Static booleans guard lazy vtable writes; audio instance/group arrays are also mutated without explicit locks. This may be acceptable if the engine guarantees single-thread ownership, but source does not assert that contract.

**Correction:** Document thread affinity. If concurrent use is allowed, use one-time vtable initialization and synchronize instance/group state. Avoid holding global locks during blocking I/O/decode.

**Tests:** Concurrent first construction and concurrent play/stop/group-load/gain operations under a race-enabled host harness if separable from XDK headers.

### R-07 — path normalization and base-path truncation

**Classification:** Confirmed path-handling defect; target impact needs XDK verification.  
**Location:** `xdkResolvePath()`, `XdkFileSystem_create()`, `src/xbox360-xdk/xdk_file_system.cpp`, approximately lines 10–30 and 331–375.

Only relative suffix separators are normalized; the base prefix may retain mixed slash styles. The 512-byte base buffer silently truncates long paths. The absolute-path test also treats any colon as absolute.

**Correction:** Reject overlong paths; normalize the entire joined path; recognize only explicit drive/device roots appropriate to XDK. Decide and document whether relative `..` traversal is permitted.

**Tests:** Both slash styles, drive/device roots, colon in relative filename, empty/trailing separators, buffer boundary, traversal and case variants.

### R-08 — case sensitivity is delegated to the target volume

**Classification:** Hypothesis / compatibility requirement.  
**Location:** `xdkResolvePath()`, `xdkFileExists()`, CreateFileA and directory operations.

The adapter does not case-fold. Actual case behavior depends on target filesystem and mounted volume; source alone cannot establish all media behavior.

**Correction:** Do not lowercase blindly. Compare target behavior with upstream overlay and real game asset paths; add a fallback only if target tests prove it necessary.

**Tests:** Mixed-case stored filename accessed with exact, uppercase and lowercase variants on each supported volume.

## 5. Ownership, WAD/AUDO and cleanup

- Main DataWin is caller-owned and stored at group index 0; audio destroy must not free it.
- Successfully parsed nonzero group DataWin objects are audio-system-owned and must be freed on destroy. NULL sparse slots mean unloaded/failed and should remain retryable.
- DataWin owns lazily loaded AUDO buffers; each audio instance owns its separately decoded PCM. PCM must remain alive until the XAudio2 voice is stopped/destroyed.
- `destroyInstance()` has the correct internal teardown order (stop/flush/destroy voice, then free PCM), but A-11 calls it on successful Start and defeats the intended lifetime.
- `DataWin_parse()` process-exit paths prevent ordinary group-load recovery; changing them requires auditing partial nested allocations and cleanup.
- Lazy AUDO short-read failure currently leaves a non-NULL buffer and blocks retry.
- XAudio2's internal audio thread does not make the adapter's instance/group structures thread-safe.

## 6. Filesystem contract review

The current FileSystem interface has 20 slots and the XDK initializer assigns all 20 by named assignments. Source inspection found no binary open-mode inversion or incorrect absolute seek origin.

| Operation | Current semantics | Assessment |
|---|---|---|
| Whole-file text/binary read | Existing file; exact-read loop; NULL on read/allocation failure | Basic behavior sound; F-03 size edge |
| Whole-file write | Create/truncate; exact-write loop; checks zero progress | Basic behavior sound |
| Binary READ | OPEN_EXISTING | Correct |
| Binary WRITE | CREATE_ALWAYS, truncates | Correct |
| Binary READWRITE | OPEN_ALWAYS, preserves existing bytes | Correct |
| Binary read/write | Actual byte count; EOF/zero request 0; OS error -1 | Callers must handle short transfers |
| Tell/seek/size | OS cursor APIs; signed 32-bit interface | F-02 boundary |
| Rewrite | Seek zero then truncate; current code checks errors | F-01 fixed; interface remains void |
| Directory listing | Allocated names plus stb_ds array | F-04 partial-list behavior |
| Rename/delete/directories | XDK/Win32 APIs | Verify overwrite/cross-volume/nested behavior on target |

Check every directory-list caller frees each name and the array with the expected allocator. No mismatch was confirmed in this pass, but it is a useful leak test.

## 7. Implementation priority

1. **P0:** Fix A-11: return the live instance ID and retain the successful voice/PCM.
2. **P0:** Fix A-16: make input-driven DataWin parser failures recoverable and clean partial state.
3. **P1:** Fix A-12 with monotonic IDs and defined wraparound.
4. **P1:** Fix R-04 lazy AUDO failure/retry and establish threading policy.
5. **P1:** Implement A-13/A-14 metadata and track-position parity.
6. **P1:** Harden F-02/F-03/F-04/R-07 integer, allocation, enumeration and path boundaries.
7. **P2:** Explicitly define capability behavior for streaming, encoded playback, 3D spatial audio and listener gain.
8. **P2:** Verify case sensitivity and concurrency contracts on target.

## 8. Regression test plan

- Playback lifecycle: embedded OGG/WAV, nonnegative ID, active voice after return, natural completion cleanup, stop during playback, no premature PCM free.
- Instance IDs: slot reuse must not let stale IDs control new voices; test wraparound/reserved ranges.
- Decoder/parser: reordered/padded WAV chunks, malformed RIFF, truncated OGG, allocation failures, invalid sample rate/channel count.
- Groups: valid/missing/empty/malformed/restored group; process survives, failed load is retryable.
- Gain/pitch: SOND pitch, multi-instance sound-index controls, per-instance controls, timed group fades, out-of-range frequency ratios.
- Metadata/timeline: duration before/after playback, position by instance and SOND, looping cursor, seek boundaries/invalid values.
- AUDO lazy loading: short read, seek failure, retry, concurrent distinct entries, correct shutdown ownership.
- Binary I/O: all open modes, partial transfers, EOF, invalid handle, negative counts, tell/seek/size limits, rewrite failure.
- Text/list: maximum accepted size, terminator overflow, allocation failure during enumeration, terminal enumeration errors, caller frees.
- Paths/case: slash styles, path buffer limit, drive/device roots, traversal policy, case variants and mounted devices.
- Concurrency: concurrent construction and shared audio/filesystem state if supported by the engine contract.

## 9. Confidence and limits

**Confirmed from current source:** successful Start is followed by unconditional instance destruction; IDs are slot-derived/reused; duration and track-position lookup are incomplete; seek/spatial/listener/stream/encoded operations are incomplete; DataWin parser has process-exit paths; lazy AUDO loading lacks seek/tell checks and preserves failed buffers; binary tell narrows to signed 32-bit; text size-plus-terminator can overflow; directory enumeration can silently omit entries; path normalization/truncation behavior.

**Hypotheses / require target or runtime-contract evidence:** concurrent lazy AUDO reads occur in normal operation; same-handle calls are concurrent; CRT fopen fails for supported Xbox device paths; actual case behavior on every volume; exact XAudio2 pitch bounds; memory pressure on representative game workloads.

**Fixed in current snapshot:** original WAV out-of-bounds copy, unchecked external read/PCM allocation paths, group false-success state, missing group-gain update, most sound-index pitch/gain gaps, and unchecked binary rewrite operations. Do not report these as still-present without evidence of regression.

**Not performed:** no build, compilation, XEX generation, emulator/console execution, audio playback, filesystem runtime test, profiler measurement or fault injection. This is a static audit, not a compatibility certification.
