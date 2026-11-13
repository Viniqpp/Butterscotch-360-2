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

### A-04 — Timed audio-group gain fades never progress

**Severity: Medium — audio mix behavior incorrect**  
**Location:** `src/xbox360-xdk/xaudio2_audio.cpp`, `xdkAudioUpdate()` lines 148-188 and `xdkSetGroupGain()` lines 687-694.  
[Update source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp#L148-L188) · [Setter source](https://github.com/Viniqpp/Butterscotch-360-2/blob/5ac4091448877fc1ce738f69d77b0c64df956925/src/xbox360-xdk/xaudio2_audio.cpp#L687-L694)

`xdkSetGroupGain()` calls `AudioSystem_setGroupGain()`, which stores the fade target and timer. Unlike the modern miniaudio backend, `xdkAudioUpdate()` never calls `AudioSystem_updateGroupGains()`. Consequently, a nonzero `timeMs` leaves the group's current gain unchanged indefinitely. The setter's immediate reapplication only uses that unchanged current gain.

**Recommended correction:** Advance group gains from `xdkAudioUpdate()`; when a gain changes, reapply the effective group gain to every active voice in that group. Keep the update on the same thread as instance bookkeeping or protect shared state.

**Suggested test:** Fade a group from 1.0 to 0.0 over a known interval; sample effective voice volume during the fade and after completion. Verify immediate (zero-duration) changes still work.

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
