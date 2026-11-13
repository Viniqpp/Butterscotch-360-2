#include <xtl.h>
#include <xaudio2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <float.h>

// stb_vorbis on big-endian Xbox 360:
// Disable the fast float-to-int path which has endian assumptions.
// Use the slow but portable path instead.
#define STB_VORBIS_NO_FAST_SCALED_FLOAT
#include "stb_vorbis.c"

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "utils.h"
#include "fmod/fmod.h"
#include "xaudio2_audio.h"

extern "C" unsigned long __cdecl DbgPrint(const char* format, ...);

// ===[ Sound Instance ]===

struct XdkSoundInstance {
    bool active;
    bool paused;
    bool loop;
    int32_t soundIndex;
    int32_t instanceId;
    int32_t priority;

    IXAudio2SourceVoice* pVoice;
    uint8_t* pcmData;       // decoded PCM (owned)
    uint32_t pcmSize;       // in bytes
    uint32_t sampleRate;
    uint16_t channels;

    float currentGain;
    float targetGain;
    float startGain;
    float fadeTotalTime;
    float fadeTimeRemaining;
    float pitch;
    float sondVolume;
    float sondPitch;
};

struct XdkInstanceArray {
    XdkSoundInstance instances[XDK_MAX_SOUND_INSTANCES];
};

static inline XdkInstanceArray* Instances(XdkAudioSystem* xa) {
    return (XdkInstanceArray*)xa->instanceData;
}

static XdkSoundInstance* findFreeSlot(XdkAudioSystem* xa) {
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (!arr->instances[i].active) return &arr->instances[i];
    }
    return NULL;
}

static XdkSoundInstance* findById(XdkAudioSystem* xa, int32_t id) {
    if (id < XDK_SOUND_INSTANCE_ID_BASE) return NULL;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && inst->instanceId == id) return inst;
    }
    return NULL;
}

// Allocate monotonically increasing public IDs rather than deriving them from
// reusable instance slots. Wrap safely while avoiding IDs held by active voices.
static int32_t allocateInstanceId(XdkAudioSystem* xa) {
    const int32_t maxCounter = 0x7fffffff - XDK_SOUND_INSTANCE_ID_BASE;
    XdkInstanceArray* arr = Instances(xa);
    for (int attempt = 0; attempt <= XDK_MAX_SOUND_INSTANCES; attempt++) {
        if (xa->nextInstanceCounter < 0 || xa->nextInstanceCounter >= maxCounter)
            xa->nextInstanceCounter = 0;
        int32_t candidate = XDK_SOUND_INSTANCE_ID_BASE + xa->nextInstanceCounter++;
        bool inUse = false;
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active && arr->instances[i].instanceId == candidate) {
                inUse = true;
                break;
            }
        }
        if (!inUse) return candidate;
    }
    return -1;
}

static void destroyInstance(XdkSoundInstance* inst) {
    if (inst->pVoice) {
        inst->pVoice->Stop();
        inst->pVoice->FlushSourceBuffers();
        inst->pVoice->DestroyVoice();
        inst->pVoice = NULL;
    }
    if (inst->pcmData) {
        free(inst->pcmData);
        inst->pcmData = NULL;
    }
    inst->active = false;
}


static bool xdkApplyInstancePitch(XdkSoundInstance* inst, float pitch) {
    // Reject NaN, infinities, zero and negative ratios before reaching XAudio2.
    if (!(pitch > 0.0f) || pitch > FLT_MAX) return false;
    if (inst && inst->pVoice) {
        HRESULT hr = inst->pVoice->SetFrequencyRatio(pitch);
        if (FAILED(hr)) {
            DbgPrint("BS: SetFrequencyRatio failed: hr=0x%08X ratio=%d/1000\n",
                     (unsigned)hr, (int)(pitch * 1000.0f));
            return false;
        }
    }
    if (inst) inst->pitch = pitch;
    return inst != NULL;
}

static void xdkApplyInstanceVolume(AudioSystem* audio, XdkSoundInstance* inst) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (inst && inst->pVoice) {
        float groupGain = AudioSystem_soundGroupGain(audio, inst->soundIndex);
        inst->pVoice->SetVolume(inst->currentGain * inst->sondVolume * xa->masterGain * groupGain);
    }
}

// ===[ Vtable Implementations ]===

static void xdkAudioInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    xa->base.dw = dataWin;
    arrput(xa->base.audioGroups, dataWin);
    xa->fileSystem = fileSystem;
    xa->masterGain = 1.0f;

    HRESULT hr = XAudio2Create((IXAudio2**)&xa->pXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        OutputDebugStringA("XAudio2Create failed\n");
        return;
    }

    IXAudio2* pXA = (IXAudio2*)xa->pXAudio2;
    hr = pXA->CreateMasteringVoice((IXAudio2MasteringVoice**)&xa->pMasterVoice,
        XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, 0, NULL);
    if (FAILED(hr)) {
        DbgPrint("BS: CreateMasteringVoice failed: 0x%08X\n", (unsigned)hr);
        return;
    }
    DbgPrint("BS: MasteringVoice created OK\n");

    xa->initialized = true;
    (void)fileSystem;
}

static void xdkAudioDestroy(AudioSystem* audio) {
    // FMOD banks/events may own decoded samples used by audio voices. Release
    // them before tearing down XAudio2 instances and the file/group resources.
    Fmod_audioDestroy(audio);

    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);


    if (arr) {
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active) destroyInstance(&arr->instances[i]);
        }
        free(arr);
    }

    for (int32_t i = 1; i < (int32_t)arrlen(audio->audioGroups); ++i) {
        if (audio->audioGroups[i]) DataWin_free(audio->audioGroups[i]);
    }
    arrfree(audio->audioGroups);
    free(audio->groupGains);

    if (xa->pMasterVoice) ((IXAudio2MasteringVoice*)xa->pMasterVoice)->DestroyVoice();
    if (xa->pXAudio2) ((IXAudio2*)xa->pXAudio2)->Release();
    free(xa);
}

static void xdkAudioUpdate(AudioSystem* audio, float deltaTime) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!xa->initialized) return;

    bool groupGainsChanged = AudioSystem_updateGroupGains(audio, deltaTime);
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active) continue;

        // Gain fading
        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (inst->fadeTimeRemaining <= 0.0f) {
                inst->currentGain = inst->targetGain;
                inst->fadeTimeRemaining = 0.0f;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
            if (inst->pVoice) {
                xdkApplyInstanceVolume(audio, inst);
            }
        }

        // Clean up instances with no voice (failed to create/decode)
        if (!inst->pVoice) {
            inst->active = false;
            continue;
        }

        if (groupGainsChanged) xdkApplyInstanceVolume(audio, inst);

        // Check if playback finished
        XAUDIO2_VOICE_STATE state;
        inst->pVoice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0 && !inst->loop) {
            destroyInstance(inst);
        }
    }
}

static void xdkGroupLoad(AudioSystem* audio, int32_t groupIndex);
static bool xdkGroupIsLoaded(AudioSystem* audio, int32_t groupIndex);


// Read little-endian WAV fields without alignment or signed-shift assumptions.
static uint16_t xdkReadLE16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t xdkReadLE32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t xdkPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!xa->initialized) { DbgPrint("BS: audio not initialized\n"); return -1; }

    DataWin* dw = xa->base.dw;
    if (soundIndex < 0 || (uint32_t)soundIndex >= dw->sond.count) return -1;

    XdkSoundInstance* inst = findFreeSlot(xa);
    if (!inst) return -1;

    memset(inst, 0, sizeof(XdkSoundInstance));
    inst->active = true;
    inst->soundIndex = soundIndex;
    inst->priority = priority;
    inst->loop = loop;
    inst->pitch = 1.0f;
    inst->sondPitch = 1.0f;
    inst->currentGain = 1.0f;
    inst->targetGain = 1.0f;
    inst->sondVolume = 1.0f;

    inst->instanceId = allocateInstanceId(xa);
    if (inst->instanceId < 0) {
        inst->active = false;
        return -1;
    }

    Sound* sound = &dw->sond.sounds[soundIndex];
    DbgPrint("BS: playSound idx=%d name=%s flags=0x%x audioFile=%d\n",
        soundIndex, sound->name ? sound->name : "?", (unsigned)sound->flags, sound->audioFile);
    if (sound->volume >= 0) inst->sondVolume = sound->volume;
    // SOND pitch is the initial playback pitch; later instance/sound-index
    // pitch changes replace this effective value, matching the upstream API.
    if (sound->pitch > 0.0f && sound->pitch <= FLT_MAX) inst->pitch = sound->pitch;

    int32_t groupIndex = sound->audioGroup < 0 ? 0 : sound->audioGroup;
    if (groupIndex > 0 && !xdkGroupIsLoaded(audio, groupIndex))
        xdkGroupLoad(audio, groupIndex);
    if (groupIndex > 0 && !xdkGroupIsLoaded(audio, groupIndex)) {
        DbgPrint("BS: audio group %d is not loaded; refusing sound %d\n", groupIndex, soundIndex);
        destroyInstance(inst);
        return -1;
    }
    DataWin* audioData = groupIndex == 0 ? dw : audio->audioGroups[groupIndex];

    // Modern WAD flags distinguish regular external audio from AUDO data.
    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    bool inAudo = !isRegular || isEmbedded || isCompressed;

    uint8_t* oggData = NULL;
    int oggSize = 0;
    bool freeOggData = false;

    if (inAudo) {
        if (sound->audioFile < 0 || (uint32_t)sound->audioFile >= audioData->audo.count) {
            DbgPrint("BS: bad audioFile idx %d (audo count=%d)\n", sound->audioFile, (int)audioData->audo.count);
            destroyInstance(inst);
            return -1;
        }
        DataWin_loadAudoIfNeeded(audioData, (uint32_t)sound->audioFile);
        AudioEntry* entry = &audioData->audo.entries[sound->audioFile];
        if (!entry->data || entry->dataSize == 0) {
            DbgPrint("BS: empty audio entry\n");
            destroyInstance(inst);
            return -1;
        }
        if (entry->dataSize > 0x7fffffffU) {
            DbgPrint("BS: embedded audio entry is too large\n");
            destroyInstance(inst);
            return -1;
        }
        oggData = entry->data;
        oggSize = (int)entry->dataSize;
    } else {
        // External: load .ogg file via FileSystem
        const char* file = sound->file;
        if (!file || !file[0] || !xa->fileSystem) {
            DbgPrint("BS: external sound: no file path or no filesystem\n");
            destroyInstance(inst);
            return -1;
        }
        bool hasExt = (strchr(file, '.') != NULL);
        char filename[512];
        if (hasExt) snprintf(filename, sizeof(filename), "%s", file);
        else snprintf(filename, sizeof(filename), "%s.ogg", file);

        char* fullPath = xa->fileSystem->vtable->resolvePath(xa->fileSystem, filename);
        if (!fullPath) { DbgPrint("BS: could not resolve path for %s\n", filename); destroyInstance(inst);
            return -1; }
        DbgPrint("BS: loading external audio: %s\n", fullPath);
        FILE* f = fopen(fullPath, "rb");
        free(fullPath);
        if (!f) { DbgPrint("BS: fopen failed for external audio\n"); destroyInstance(inst);
            return -1; }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); destroyInstance(inst); return -1; }
        long fileLength = ftell(f);
        if (fileLength <= 0 || fileLength > 0x7fffffffL || fseek(f, 0, SEEK_SET) != 0) {
            fclose(f); destroyInstance(inst); return -1;
        }
        oggSize = (int)fileLength;
        oggData = (uint8_t*)malloc((size_t)oggSize);
        if (!oggData) { fclose(f); destroyInstance(inst); return -1; }
        size_t bytesRead = fread(oggData, 1, (size_t)oggSize, f);
        fclose(f);
        if (bytesRead != (size_t)oggSize) {
            free(oggData); destroyInstance(inst); return -1;
        }
        freeOggData = true;
    }

    // Decode either checked PCM WAV or OGG/Vorbis.
    int channels = 0, sampleRate = 0, sampleCount = 0;
    short* pcm = NULL;

    if (!oggData || oggSize <= 0) {
        if (freeOggData) free(oggData);
        destroyInstance(inst);
        return -1;
    }

    if (oggSize >= 12 && memcmp(oggData, "RIFF", 4) == 0) {
        uint32_t riffBytes = xdkReadLE32(oggData + 4);
        if (memcmp(oggData + 8, "WAVE", 4) != 0 || riffBytes < 4 ||
            riffBytes > (uint32_t)(oggSize - 8)) {
            DbgPrint("BS: invalid RIFF/WAVE header\n");
            if (freeOggData) free(oggData);
            destroyInstance(inst);
            return -1;
        }

        size_t offset = 12;
        size_t riffEnd = (size_t)riffBytes + 8;
        uint16_t formatTag = 0, bitsPerSample = 0, blockAlign = 0, wavChannels = 0;
        uint32_t wavRate = 0, dataSize = 0;
        size_t dataOffset = 0;
        bool haveFmt = false, haveData = false;
        bool malformedChunk = false;

        while (offset <= riffEnd && riffEnd - offset >= 8) {
            const uint8_t* chunk = oggData + offset;
            uint32_t chunkBytes = xdkReadLE32(chunk + 4);
            size_t payload = offset + 8;
            if ((size_t)chunkBytes > riffEnd - payload) {
                malformedChunk = true;
                break;
            }

            if (memcmp(chunk, "fmt ", 4) == 0) {
                if (haveFmt || chunkBytes < 16) {
                    malformedChunk = true;
                    break;
                }
                formatTag = xdkReadLE16(oggData + payload);
                wavChannels = xdkReadLE16(oggData + payload + 2);
                wavRate = xdkReadLE32(oggData + payload + 4);
                blockAlign = xdkReadLE16(oggData + payload + 12);
                bitsPerSample = xdkReadLE16(oggData + payload + 14);
                haveFmt = true;
            } else if (memcmp(chunk, "data", 4) == 0) {
                if (haveData) {
                    malformedChunk = true;
                    break;
                }
                dataOffset = payload;
                dataSize = chunkBytes;
                haveData = true;
            }

            size_t paddedBytes = (size_t)chunkBytes + (chunkBytes & 1U);
            if (paddedBytes > riffEnd - payload) {
                malformedChunk = true;
                break;
            }
            offset = payload + paddedBytes;
        }
        if (offset != riffEnd) malformedChunk = true;

        if (malformedChunk || !haveFmt || !haveData || formatTag != 1 || bitsPerSample != 16 ||
            wavChannels == 0 || wavChannels > 2 || wavRate == 0 ||
            blockAlign != (uint16_t)(wavChannels * 2) ||
            dataOffset > (size_t)oggSize || dataSize > (size_t)oggSize - dataOffset ||
            dataSize == 0 || (dataSize % blockAlign) != 0 ||
            dataSize > 0x7fffffffU) {
            DbgPrint("BS: unsupported or malformed PCM WAV\n");
            if (freeOggData) free(oggData);
            destroyInstance(inst);
            return -1;
        }

        channels = (int)wavChannels;
        sampleRate = (int)wavRate;
        sampleCount = (int)(dataSize / blockAlign);
        pcm = (short*)malloc((size_t)dataSize);
        if (!pcm) {
            if (freeOggData) free(oggData);
            destroyInstance(inst);
            return -1;
        }
        memcpy(pcm, oggData + dataOffset, (size_t)dataSize);
        for (uint32_t sample = 0; sample < dataSize / 2; ++sample) {
            uint16_t value = (uint16_t)pcm[sample];
            pcm[sample] = (short)((value >> 8) | (value << 8));
        }
        DbgPrint("BS: WAV: ch=%d rate=%d samples=%d\n", channels, sampleRate, sampleCount);
    } else {
        int stbErr = 0;
        stb_vorbis* vorbis = stb_vorbis_open_memory(oggData, oggSize, &stbErr, NULL);
        if (!vorbis) {
            DbgPrint("BS: vorbis open failed: err=%d size=%d\n", stbErr, oggSize);
            if (freeOggData) free(oggData);
            destroyInstance(inst);
            return -1;
        }
        stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        int totalSamples = stb_vorbis_stream_length_in_samples(vorbis);
        channels = info.channels;
        sampleRate = (int)info.sample_rate;
        if (channels <= 0 || channels > 2 || sampleRate <= 0 || totalSamples <= 0 ||
            (unsigned long long)totalSamples * (unsigned long long)channels * sizeof(short) > 0x7fffffffULL) {
            stb_vorbis_close(vorbis);
            if (freeOggData) free(oggData);
            destroyInstance(inst);
            return -1;
        }
        size_t pcmBytes = (size_t)totalSamples * (size_t)channels * sizeof(short);
        pcm = (short*)malloc(pcmBytes);
        if (!pcm) {
            stb_vorbis_close(vorbis);
            if (freeOggData) free(oggData);
            destroyInstance(inst);
            return -1;
        }
        sampleCount = stb_vorbis_get_samples_short_interleaved(vorbis, channels, pcm, totalSamples * channels);
        stb_vorbis_close(vorbis);
        if (sampleCount <= 0) {
            free(pcm);
            if (freeOggData) free(oggData);
            destroyInstance(inst);
            return -1;
        }
        DbgPrint("BS: OGG: ch=%d rate=%d samples=%d\n", channels, sampleRate, sampleCount);
    }

    if (freeOggData) free(oggData);

    if (sampleCount <= 0 || !pcm || sampleRate <= 0 || channels <= 0 || channels > 2) {
        free(pcm);
        destroyInstance(inst);
        return -1;
    }

    inst->sampleRate = (uint32_t)sampleRate;
    inst->channels = (uint16_t)channels;
    inst->pcmSize = (uint32_t)(sampleCount * channels * sizeof(short));
    inst->pcmData = (uint8_t*)pcm;

    // Xbox 360 XAudio2 expects big-endian PCM samples.
    // WAV samples were already byte-swapped during parsing above.
    // OGG samples from stb_vorbis: the int16 conversion uses native endian,
    // which is already big-endian on Xbox 360. No additional swap needed.

    // Create XAudio2 source voice using WAVEFORMATEXTENSIBLE
    WAVEFORMATEXTENSIBLE wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = (WORD)channels;
    wfx.Format.nSamplesPerSec = (DWORD)sampleRate;
    wfx.Format.wBitsPerSample = 16;
    wfx.Format.nBlockAlign = (WORD)(channels * 2);
    wfx.Format.nAvgBytesPerSec = (DWORD)(sampleRate * channels * 2);
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 16;
    wfx.dwChannelMask = (channels == 1) ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    IXAudio2* pXA = (IXAudio2*)xa->pXAudio2;
    HRESULT hr = pXA->CreateSourceVoice(&inst->pVoice, (WAVEFORMATEX*)&wfx);
    if (FAILED(hr) || !inst->pVoice) {
        DbgPrint("BS: CreateSourceVoice failed: hr=0x%08X\n", (unsigned)hr);
        destroyInstance(inst);
        return -1;
    }

    // Submit PCM buffer
    XAUDIO2_BUFFER buf;
    memset(&buf, 0, sizeof(buf));
    buf.Flags = XAUDIO2_END_OF_STREAM;
    buf.AudioBytes = inst->pcmSize;
    buf.pAudioData = inst->pcmData;
    if (loop) {
        buf.LoopCount = XAUDIO2_LOOP_INFINITE;
        buf.Flags = 0; // don't signal end if looping
    }

    hr = inst->pVoice->SubmitSourceBuffer(&buf);
    if (FAILED(hr)) {
        DbgPrint("BS: SubmitSourceBuffer failed: hr=0x%08X\n", (unsigned)hr);
        destroyInstance(inst);
        return -1;
    }
    hr = inst->pVoice->SetVolume(inst->currentGain * inst->sondVolume * xa->masterGain *
                                  AudioSystem_soundGroupGain(audio, soundIndex));
    if (FAILED(hr)) {
        DbgPrint("BS: SetVolume failed: hr=0x%08X\n", (unsigned)hr);
        destroyInstance(inst);
        return -1;
    }
    if (!xdkApplyInstancePitch(inst, inst->pitch)) {
        destroyInstance(inst);
        return -1;
    }
    hr = inst->pVoice->Start(0);
    if (FAILED(hr)) {
        DbgPrint("BS: Start failed: hr=0x%08X\n", (unsigned)hr);
        destroyInstance(inst);
        return -1;
    }

    // The source voice now owns a reference to pcmData. Keep the instance
    // active until the voice completes or the caller explicitly stops it.
    return inst->instanceId;
}

static void xdkStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);

    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) destroyInstance(inst);
    } else {
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance)
                destroyInstance(&arr->instances[i]);
        }
    }
}

static void xdkStopAll(AudioSystem* audio) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active) destroyInstance(&arr->instances[i]);
    }
}

static bool xdkIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);

    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        return inst && !inst->paused;
    }
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance && !arr->instances[i].paused)
            return true;
    }
    return false;
}

static void xdkPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);

    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) { inst->paused = true; if (inst->pVoice) inst->pVoice->Stop(); }
    } else {
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance) {
                arr->instances[i].paused = true;
                if (arr->instances[i].pVoice) arr->instances[i].pVoice->Stop();
            }
        }
    }
}

static void xdkResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);

    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) { inst->paused = false; if (inst->pVoice) inst->pVoice->Start(0); }
    } else {
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance) {
                arr->instances[i].paused = false;
                if (arr->instances[i].pVoice) arr->instances[i].pVoice->Start(0);
            }
        }
    }
}

static void xdkPauseAll(AudioSystem* audio) {
    XdkInstanceArray* arr = Instances((XdkAudioSystem*)audio);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active) {
            arr->instances[i].paused = true;
            if (arr->instances[i].pVoice) arr->instances[i].pVoice->Stop();
        }
    }
}

static void xdkResumeAll(AudioSystem* audio) {
    XdkInstanceArray* arr = Instances((XdkAudioSystem*)audio);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active) {
            arr->instances[i].paused = false;
            if (arr->instances[i].pVoice) arr->instances[i].pVoice->Start(0);
        }
    }
}

static void xdkApplyGainTarget(XdkAudioSystem* xa, AudioSystem* audio,
                                XdkSoundInstance* inst, float gain, uint32_t timeMs) {
    (void)xa;
    if (timeMs == 0) {
        inst->currentGain = gain;
        inst->targetGain = gain;
        inst->fadeTimeRemaining = 0.0f;
        xdkApplyInstanceVolume(audio, inst);
    } else {
        inst->startGain = inst->currentGain;
        inst->targetGain = gain;
        inst->fadeTotalTime = (float)timeMs / 1000.0f;
        inst->fadeTimeRemaining = inst->fadeTotalTime;
    }
}

static void xdkSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) xdkApplyGainTarget(xa, audio, inst, gain, timeMs);
        return;
    }
    if (soundOrInstance < 0) return;

    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance)
            xdkApplyGainTarget(xa, audio, inst, gain, timeMs);
    }
}

static float xdkGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        return inst ? inst->currentGain : 0.0f;
    }
    if (soundOrInstance < 0) return 0.0f;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) return inst->currentGain;
    }
    return 0.0f;
}

static void xdkSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!(pitch > 0.0f) || pitch > FLT_MAX) return;
    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) xdkApplyInstancePitch(inst, pitch);
        return;
    }
    if (soundOrInstance < 0) return;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance)
            xdkApplyInstancePitch(inst, pitch);
    }
}

static float xdkGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        return inst ? inst->pitch : 1.0f;
    }
    if (soundOrInstance < 0) return 1.0f;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance) return inst->pitch;
    }
    return 1.0f;
}

static float xdkGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst && inst->pVoice && inst->sampleRate > 0) {
            XAUDIO2_VOICE_STATE state;
            inst->pVoice->GetState(&state, 0);
            return (float)state.SamplesPlayed / (float)inst->sampleRate;
        }
    }
    return 0.0f;
}

static void xdkSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    (void)audio; (void)soundOrInstance; (void)positionSeconds;
    // TODO: seek support requires re-submitting buffer from offset
}

static void xdkSetMasterGain(AudioSystem* audio, float gain) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    xa->masterGain = gain;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active) xdkApplyInstanceVolume(audio, &arr->instances[i]);
    }
}

static void xdkSetChannelCount(AudioSystem* audio, int32_t count) {
    (void)audio; (void)count;
}

static void xdkGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    if (groupIndex <= 0 || !audio->dw || (uint32_t)groupIndex >= audio->dw->agrp.count) {
        DbgPrint("BS: requested invalid audio group %d\n", groupIndex);
        return;
    }

    // Preserve sparse group indices with NULL entries. NULL means "not loaded"
    // and remains retryable; a parsed DataWin is the only loaded state.
    while (arrlen(audio->audioGroups) <= groupIndex) {
        DataWin* empty = NULL;
        arrput(audio->audioGroups, empty);
    }
    if (audio->audioGroups[groupIndex] != NULL) return;

    AudioGroup* group = &audio->dw->agrp.audioGroups[groupIndex];
    char defaultPath[64];
    const char* relativePath = group->path;
    if (!relativePath || !relativePath[0]) {
        sprintf(defaultPath, "audiogroup%d.dat", groupIndex);
        relativePath = defaultPath;
    }

    FileSystem* fs = ((XdkAudioSystem*)audio)->fileSystem;
    if (!fs || !fs->vtable->fileExists(fs, relativePath)) {
        DbgPrint("BS: audio group %d missing: %s\n", groupIndex, relativePath);
        return;
    }

    char* fullPath = fs->vtable->resolvePath(fs, relativePath);
    if (!fullPath) return;

    DataWinParserOptions options;
    memset(&options, 0, sizeof(options));
    options.parseAudo = true;
    options.lazyLoadAudio = audio->dw->lazyLoadAudio;
    if (audio->dw->mappedFile) options.loadType = DATAWINLOADTYPE_MAP_FILE;
    DataWin* groupData = DataWin_parse(fullPath, options);
    free(fullPath);
    if (!groupData) {
        DbgPrint("BS: audio group %d parse failed: %s\n", groupIndex, relativePath);
        return;
    }

    audio->audioGroups[groupIndex] = groupData;
}

static bool xdkGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    return groupIndex >= 0 && (uint32_t)groupIndex < arrlen(audio->audioGroups) &&
           audio->audioGroups[groupIndex] != NULL;
}


static void xdkSetSoundSpatial(AudioSystem* audio, int32_t instanceId,
                               float x, float y, float z, float vx, float vy, float vz,
                               float ref, float max, float factor) {
    // XAudio2 spatial routing is not configured by this stereo backend yet.
    (void)audio; (void)instanceId; (void)x; (void)y; (void)z;
    (void)vx; (void)vy; (void)vz; (void)ref; (void)max; (void)factor;
}

static void xdkSetListenerPosition(AudioSystem* audio, float x, float y, float z) {
    audio->listenerX = x;
    audio->listenerY = y;
    audio->listenerZ = z;
}

static void xdkSuspend(AudioSystem* audio) {
    xdkPauseAll(audio);
}

static void xdkResume(AudioSystem* audio) {
    xdkResumeAll(audio);
}

static float xdkGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkSoundInstance* inst = soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE
        ? findById(xa, soundOrInstance) : NULL;
    if (!inst && soundOrInstance >= 0 && audio->dw &&
        (uint32_t)soundOrInstance < audio->dw->sond.count) {
        XdkInstanceArray* arr = Instances(xa);
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance) {
                inst = &arr->instances[i];
                break;
            }
        }
    }
    if (!inst || inst->sampleRate == 0 || inst->channels == 0) return 0.0f;
    return (float)inst->pcmSize / ((float)inst->sampleRate * (float)inst->channels * 2.0f);
}

static void xdkSetMasterGainForListener(AudioSystem* audio, float gain, int32_t listenerId) {
    (void)listenerId;
    xdkSetMasterGain(audio, gain);
}

static void xdkSetGroupGain(AudioSystem* audio, int32_t groupIndex, float gain, uint32_t timeMs) {
    AudioSystem_setGroupGain(audio, groupIndex, gain, timeMs);
    XdkInstanceArray* arr = Instances((XdkAudioSystem*)audio);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && AudioSystem_soundGroup(audio, inst->soundIndex) == groupIndex)
            xdkApplyInstanceVolume(audio, inst);
    }
}

static int32_t xdkCreateStream(AudioSystem* audio, const char* filename) {
    (void)audio; (void)filename;
    return -1; // Stream voices are not implemented by this decoder-backed adapter.
}

static bool xdkDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    (void)audio; (void)streamIndex;
    return false;
}

static int32_t xdkPlayEncoded(AudioSystem* audio, const uint8_t* data, size_t bytes, bool loop) {
    (void)audio; (void)data; (void)bytes; (void)loop;
    return -1; // The modern encoded-voice path requires a separate decoder/voice owner.
}

// ===[ Vtable ]===

static AudioSystemVtable xdkAudioVtable;
static bool xdkAudioVtableInitialized = false;

static void xdkInitializeAudioVtable(void) {
    if (xdkAudioVtableInitialized) return;
    memset(&xdkAudioVtable, 0, sizeof(xdkAudioVtable));
    xdkAudioVtable.init = xdkAudioInit;
    xdkAudioVtable.destroy = xdkAudioDestroy;
    xdkAudioVtable.update = xdkAudioUpdate;
    xdkAudioVtable.playSound = xdkPlaySound;
    xdkAudioVtable.setSoundSpatial = xdkSetSoundSpatial;
    xdkAudioVtable.setListenerPosition = xdkSetListenerPosition;
    xdkAudioVtable.stopSound = xdkStopSound;
    xdkAudioVtable.stopAll = xdkStopAll;
    xdkAudioVtable.isPlaying = xdkIsPlaying;
    xdkAudioVtable.pauseSound = xdkPauseSound;
    xdkAudioVtable.resumeSound = xdkResumeSound;
    xdkAudioVtable.pauseAll = xdkPauseAll;
    xdkAudioVtable.resumeAll = xdkResumeAll;
    xdkAudioVtable.suspend = xdkSuspend;
    xdkAudioVtable.resume = xdkResume;
    xdkAudioVtable.setSoundGain = xdkSetSoundGain;
    xdkAudioVtable.getSoundGain = xdkGetSoundGain;
    xdkAudioVtable.setSoundPitch = xdkSetSoundPitch;
    xdkAudioVtable.getSoundPitch = xdkGetSoundPitch;
    xdkAudioVtable.getTrackPosition = xdkGetTrackPosition;
    xdkAudioVtable.setTrackPosition = xdkSetTrackPosition;
    xdkAudioVtable.getSoundLength = xdkGetSoundLength;
    xdkAudioVtable.setMasterGain = xdkSetMasterGain;
    xdkAudioVtable.setMasterGainForListener = xdkSetMasterGainForListener;
    xdkAudioVtable.setChannelCount = xdkSetChannelCount;
    xdkAudioVtable.setGroupGain = xdkSetGroupGain;
    xdkAudioVtable.groupLoad = xdkGroupLoad;
    xdkAudioVtable.groupIsLoaded = xdkGroupIsLoaded;
    xdkAudioVtable.createStream = xdkCreateStream;
    xdkAudioVtable.destroyStream = xdkDestroyStream;
    xdkAudioVtable.playEncoded = xdkPlayEncoded;
    xdkAudioVtableInitialized = true;
}

// ===[ Public API ]===

XdkAudioSystem* XdkAudioSystem_create(void) {
    XdkAudioSystem* xa = (XdkAudioSystem*)calloc(1, sizeof(XdkAudioSystem));
    if (!xa) return NULL;

    xa->instanceData = calloc(1, sizeof(XdkInstanceArray));
    if (!xa->instanceData) {
        free(xa);
        return NULL;
    }

    xdkInitializeAudioVtable();
    xa->base.vtable = &xdkAudioVtable;
    xa->masterGain = 1.0f;
    return xa;
}
