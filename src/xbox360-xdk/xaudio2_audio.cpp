#include <xtl.h>
#include <xaudio2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <float.h>
#include <math.h>

#define XDK_MAX_ENCODED_AUDIO_BYTES (32u * 1024u * 1024u)
#define XDK_MAX_ACTIVE_PCM_BYTES (96u * 1024u * 1024u)
#define XDK_MAX_AUDIO_SAMPLE_RATE 192000u

// stb_vorbis on big-endian Xbox 360:
// Disable the fast float-to-int path which has endian assumptions.
// Use the slow but portable path instead.
#define STB_VORBIS_NO_FAST_SCALED_FLOAT
#include "stb_vorbis.c"

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "utils.h"
#include "stb_ds.h"
#include "fmod/fmod.h"
#include "xaudio2_audio.h"


// ===[ Sound Instance ]===

struct XdkSoundInstance {
    bool active;
    bool paused;
    bool loop;
    XdkAudioSystem* owner;
    uint32_t positionOffsetSamples;
    uint64_t samplesPlayedAtSeek;
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

static bool xdkIsInstanceId(int32_t id) {
    return id >= XDK_SOUND_INSTANCE_ID_BASE && id < XDK_AUDIO_STREAM_INDEX_BASE;
}

static XdkSoundInstance* findById(XdkAudioSystem* xa, int32_t id) {
    if (!xdkIsInstanceId(id)) return NULL;
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
    const int32_t maxCounter = XDK_AUDIO_STREAM_INDEX_BASE - XDK_SOUND_INSTANCE_ID_BASE - 1;
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
        if (inst->owner) {
            if (inst->owner->activePcmBytes >= inst->pcmSize)
                inst->owner->activePcmBytes -= inst->pcmSize;
            else
                inst->owner->activePcmBytes = 0;
        }
        free(inst->pcmData);
        inst->pcmData = NULL;
    }
    inst->pcmSize = 0;
    inst->owner = NULL;
    inst->active = false;
}


static bool xdkApplyInstancePitch(XdkSoundInstance* inst, float pitch) {
    // Voices are created with XAUDIO2_MAX_FREQ_RATIO below. Reject NaN,
    // infinities and out-of-range ratios instead of storing a value that
    // XAudio2 would clamp differently from the GameMaker-visible state.
    if (!(pitch >= XAUDIO2_MIN_FREQ_RATIO && pitch <= XAUDIO2_MAX_FREQ_RATIO) || !inst)
        return false;
    if (inst->pVoice) {
        HRESULT hr = inst->pVoice->SetFrequencyRatio(pitch);
        if (FAILED(hr)) {
            logInfo("BS: SetFrequencyRatio failed: hr=0x%08X ratio=%d/1000\n",
                     (unsigned)hr, (int)(pitch * 1000.0f));
            return false;
        }
    }
    inst->pitch = pitch;
    return true;
}

static void xdkApplyInstanceVolume(AudioSystem* audio, XdkSoundInstance* inst) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (inst && inst->pVoice) {
        float groupGain = 1.0f;
        if (inst->soundIndex >= 0 && xa->base.dw &&
            (uint32_t)inst->soundIndex < xa->base.dw->sond.count) {
            groupGain = AudioSystem_soundGroupGain(audio, inst->soundIndex);
        }
        HRESULT hr = inst->pVoice->SetVolume(inst->currentGain * xa->masterGain * groupGain);
        if (FAILED(hr)) logInfo("BS: SetVolume update failed: hr=0x%08X\n", (unsigned)hr);
    }
}

static bool xdkCanAllocatePcm(XdkAudioSystem* xa, size_t bytes) {
    return xa && bytes > 0 && bytes <= xa->maxPcmBytes &&
           xa->activePcmBytes <= xa->maxPcmBytes - (uint32_t)bytes;
}

// ===[ Vtable Implementations ]===

static void xdkAudioInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    xa->base.dw = dataWin;
    arrput(xa->base.audioGroups, dataWin);
    if (arrlen(xa->base.audioGroups) != 1 || xa->base.audioGroups[0] != dataWin) {
        logInfo("BS: could not allocate main audio-group table\n");
        return;
    }
    xa->fileSystem = fileSystem;
    xa->masterGain = 1.0f;
    xa->activePcmBytes = 0;
    xa->maxPcmBytes = XDK_MAX_ACTIVE_PCM_BYTES;

    HRESULT hr = XAudio2Create((IXAudio2**)&xa->pXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !xa->pXAudio2) {
        logWarn("%s", "XAudio2Create failed\n");
        if (xa->pXAudio2) {
            ((IXAudio2*)xa->pXAudio2)->Release();
            xa->pXAudio2 = NULL;
        }
        return;
    }

    IXAudio2* pXA = (IXAudio2*)xa->pXAudio2;
    hr = pXA->CreateMasteringVoice((IXAudio2MasteringVoice**)&xa->pMasterVoice,
        XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, 0, NULL);
    if (FAILED(hr) || !xa->pMasterVoice) {
        logInfo("BS: CreateMasteringVoice failed: 0x%08X\n", (unsigned)hr);
        if (xa->pMasterVoice) {
            ((IXAudio2MasteringVoice*)xa->pMasterVoice)->DestroyVoice();
            xa->pMasterVoice = NULL;
        }
        pXA->Release();
        xa->pXAudio2 = NULL;
        return;
    }
    logInfo("BS: MasteringVoice created OK\n");

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
    for (int32_t i = 0; i < XDK_MAX_AUDIO_STREAMS; i++) {
        free(xa->streams[i].filePath);
        xa->streams[i].filePath = NULL;
        xa->streams[i].active = false;
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

        // Clean up failed/incomplete instances through the same owner-aware
        // path used everywhere else, so PCM memory accounting cannot leak.
        if (!inst->pVoice) {
            destroyInstance(inst);
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
    if (!xa->initialized) { logInfo("BS: audio not initialized\n"); return -1; }

    DataWin* dw = xa->base.dw;
    bool isStream = soundIndex >= XDK_AUDIO_STREAM_INDEX_BASE;
    Sound* sound = NULL;
    const char* externalFile = NULL;
    float initialGain = 1.0f;
    float initialPitch = 1.0f;
    DataWin* audioData = dw;
    bool inAudo = false;

    if (isStream) {
        int32_t streamSlot = soundIndex - XDK_AUDIO_STREAM_INDEX_BASE;
        if (streamSlot < 0 || streamSlot >= XDK_MAX_AUDIO_STREAMS ||
            !xa->streams[streamSlot].active || !xa->streams[streamSlot].filePath) {
            logInfo("BS: invalid audio stream index %d\n", soundIndex);
            return -1;
        }
        externalFile = xa->streams[streamSlot].filePath;
        initialGain = xa->streams[streamSlot].initialGain;
        initialPitch = xa->streams[streamSlot].initialPitch;
    } else {
        if (soundIndex < 0 || !dw || (uint32_t)soundIndex >= dw->sond.count) return -1;
        sound = &dw->sond.sounds[soundIndex];
        if (sound->volume >= 0.0f) initialGain = sound->volume;
        if (sound->pitch > 0.0f && sound->pitch <= FLT_MAX) initialPitch = sound->pitch;

        int32_t groupIndex = sound->audioGroup < 0 ? 0 : sound->audioGroup;
        if (groupIndex > 0 && !xdkGroupIsLoaded(audio, groupIndex))
            xdkGroupLoad(audio, groupIndex);
        if (groupIndex > 0 && !xdkGroupIsLoaded(audio, groupIndex)) {
            logInfo("BS: audio group %d is not loaded; refusing sound %d\n", groupIndex, soundIndex);
            return -1;
        }
        if (groupIndex < 0 || (uint32_t)groupIndex >= arrlen(audio->audioGroups) ||
            !audio->audioGroups[groupIndex]) return -1;
        audioData = audio->audioGroups[groupIndex];

        bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
        bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
        bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
        inAudo = !isRegular || isEmbedded || isCompressed;
    }

    XdkSoundInstance* inst = findFreeSlot(xa);
    if (!inst) return -1;

    memset(inst, 0, sizeof(XdkSoundInstance));
    inst->active = true;
    inst->soundIndex = soundIndex;
    inst->priority = priority;
    inst->loop = loop;
    inst->pitch = initialPitch;
    inst->sondPitch = initialPitch;
    // currentGain is the effective base gain for both SOND sounds and streams;
    // later audio_sound_gain calls replace it rather than multiplying the
    // original SOND/stream gain a second time.
    inst->currentGain = initialGain;
    inst->targetGain = initialGain;
    inst->startGain = initialGain;
    inst->sondVolume = 1.0f;

    inst->instanceId = allocateInstanceId(xa);
    if (inst->instanceId < 0) {
        inst->active = false;
        return -1;
    }


    uint8_t* oggData = NULL;
    int oggSize = 0;
    bool freeOggData = false;

    if (inAudo) {
        if (sound->audioFile < 0 || (uint32_t)sound->audioFile >= audioData->audo.count) {
            logInfo("BS: bad audioFile idx %d (audo count=%d)\n", sound->audioFile, (int)audioData->audo.count);
            destroyInstance(inst);
            return -1;
        }
        DataWin_loadAudoIfNeeded(audioData, (uint32_t)sound->audioFile);
        AudioEntry* entry = &audioData->audo.entries[sound->audioFile];
        if (!entry->data || entry->dataSize == 0) {
            logInfo("BS: empty audio entry\n");
            destroyInstance(inst);
            return -1;
        }
        if (entry->dataSize > XDK_MAX_ENCODED_AUDIO_BYTES) {
            logInfo("BS: embedded audio entry exceeds the encoded-audio memory budget\n");
            destroyInstance(inst);
            return -1;
        }
        oggData = entry->data;
        oggSize = (int)entry->dataSize;
    } else {
        // External: load .ogg file via FileSystem
        const char* file = isStream ? externalFile : sound->file;
        if (!file || !file[0] || !xa->fileSystem) {
            logInfo("BS: external sound: no file path or no filesystem\n");
            destroyInstance(inst);
            return -1;
        }
        // Detect extensions on the filename component, not a dotted parent directory.
        const char* fileName = file;
        const char* slash = strrchr(file, '/');
        const char* backslash = strrchr(file, '\\');
        if (slash && slash + 1 > fileName) fileName = slash + 1;
        if (backslash && backslash + 1 > fileName) fileName = backslash + 1;
        bool hasExt = strchr(fileName, '.') != NULL;
        size_t fileLen = strlen(file);
        size_t extensionLen = hasExt ? 0 : 4;
        if (fileLen + extensionLen >= 512) {
            logInfo("BS: external audio path is too long\n");
            destroyInstance(inst);
            return -1;
        }
        char filename[512];
        memcpy(filename, file, fileLen);
        if (!hasExt) memcpy(filename + fileLen, ".ogg", 5);
        else filename[fileLen] = '\0';

        FileSystem* fs = xa->fileSystem;
        void* handle = fs->vtable->binaryOpen(fs, filename, GML_FILE_BIN_READ);
        if (!handle) {
            logInfo("BS: FileSystem could not open external audio %s\n", filename);
            destroyInstance(inst);
            return -1;
        }
        int32_t binarySize = fs->vtable->binarySize(fs, handle);
        if (binarySize <= 0 || (uint32_t)binarySize > XDK_MAX_ENCODED_AUDIO_BYTES) {
            fs->vtable->binaryClose(fs, handle);
            logInfo("BS: external audio size is invalid or over budget (%d)\n", binarySize);
            destroyInstance(inst);
            return -1;
        }
        oggData = (uint8_t*)malloc((size_t)binarySize);
        if (!oggData) {
            fs->vtable->binaryClose(fs, handle);
            destroyInstance(inst);
            return -1;
        }
        int32_t totalRead = 0;
        while (totalRead < binarySize) {
            int32_t remaining = binarySize - totalRead;
            int32_t bytesRead = fs->vtable->binaryRead(fs, handle, oggData + totalRead, remaining);
            if (bytesRead <= 0 || bytesRead > remaining) break;
            totalRead += bytesRead;
        }
        fs->vtable->binaryClose(fs, handle);
        if (totalRead != binarySize) {
            free(oggData);
            oggData = NULL;
            logInfo("BS: short FileSystem read for external audio %s (%d/%d)\n",
                     filename, totalRead, binarySize);
            destroyInstance(inst);
            return -1;
        }
        oggSize = binarySize;
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
            logInfo("BS: invalid RIFF/WAVE header\n");
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
            wavChannels == 0 || wavChannels > 2 || wavRate == 0 || wavRate > XDK_MAX_AUDIO_SAMPLE_RATE ||
            blockAlign != (uint16_t)(wavChannels * 2) ||
            dataOffset > (size_t)oggSize || dataSize > (size_t)oggSize - dataOffset ||
            dataSize == 0 || (dataSize % blockAlign) != 0 ||
            dataSize > 0x7fffffffU) {
            logInfo("BS: unsupported or malformed PCM WAV\n");
            if (freeOggData) free(oggData);
            destroyInstance(inst);
            return -1;
        }

        if (!xdkCanAllocatePcm(xa, dataSize)) {
            logInfo("BS: WAV PCM exceeds active memory budget (requested=%u active=%u limit=%u)\n",
                     dataSize, xa->activePcmBytes, xa->maxPcmBytes);
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
    } else {
        int stbErr = 0;
        stb_vorbis* vorbis = stb_vorbis_open_memory(oggData, oggSize, &stbErr, NULL);
        if (!vorbis) {
            logInfo("BS: vorbis open failed: err=%d size=%d\n", stbErr, oggSize);
            if (freeOggData) free(oggData);
            destroyInstance(inst);
            return -1;
        }
        stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        int totalSamples = stb_vorbis_stream_length_in_samples(vorbis);
        channels = info.channels;
        sampleRate = (int)info.sample_rate;
        if (channels <= 0 || channels > 2 || sampleRate <= 0 || sampleRate > (int)XDK_MAX_AUDIO_SAMPLE_RATE || totalSamples <= 0 ||
            (unsigned long long)totalSamples * (unsigned long long)channels * sizeof(short) > 0x7fffffffULL) {
            stb_vorbis_close(vorbis);
            if (freeOggData) free(oggData);
            destroyInstance(inst);
            return -1;
        }
        size_t pcmBytes = (size_t)totalSamples * (size_t)channels * sizeof(short);
        if (!xdkCanAllocatePcm(xa, pcmBytes)) {
            logInfo("BS: OGG PCM exceeds active memory budget (requested=%u active=%u limit=%u)\n",
                     (unsigned)pcmBytes, xa->activePcmBytes, xa->maxPcmBytes);
            stb_vorbis_close(vorbis);
            if (freeOggData) free(oggData);
            destroyInstance(inst);
            return -1;
        }
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
        logInfo("BS: OGG: ch=%d rate=%d samples=%d\n", channels, sampleRate, sampleCount);
    }

    if (freeOggData) free(oggData);

    if (sampleCount <= 0 || !pcm || sampleRate <= 0 || channels <= 0 || channels > 2) {
        free(pcm);
        destroyInstance(inst);
        return -1;
    }

    uint32_t decodedPcmSize = (uint32_t)(sampleCount * channels * sizeof(short));
    if (decodedPcmSize == 0 || decodedPcmSize > xa->maxPcmBytes ||
        xa->activePcmBytes > xa->maxPcmBytes - decodedPcmSize) {
        logInfo("BS: decoded audio exceeds active PCM budget (requested=%u active=%u limit=%u)\n",
                 decodedPcmSize, xa->activePcmBytes, xa->maxPcmBytes);
        free(pcm);
        destroyInstance(inst);
        return -1;
    }

    inst->sampleRate = (uint32_t)sampleRate;
    inst->channels = (uint16_t)channels;
    inst->pcmSize = decodedPcmSize;
    inst->pcmData = (uint8_t*)pcm;
    inst->owner = xa;
    xa->activePcmBytes += decodedPcmSize;

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
    HRESULT hr = pXA->CreateSourceVoice(&inst->pVoice, (WAVEFORMATEX*)&wfx, 0, XAUDIO2_MAX_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr) || !inst->pVoice) {
        logInfo("BS: CreateSourceVoice failed: hr=0x%08X\n", (unsigned)hr);
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
        logInfo("BS: SubmitSourceBuffer failed: hr=0x%08X\n", (unsigned)hr);
        destroyInstance(inst);
        return -1;
    }
    float groupGain = !isStream ? AudioSystem_soundGroupGain(audio, soundIndex) : 1.0f;
    hr = inst->pVoice->SetVolume(inst->currentGain * xa->masterGain * groupGain);
    if (FAILED(hr)) {
        logInfo("BS: SetVolume failed: hr=0x%08X\n", (unsigned)hr);
        destroyInstance(inst);
        return -1;
    }
    if (!xdkApplyInstancePitch(inst, inst->pitch)) {
        destroyInstance(inst);
        return -1;
    }
    hr = inst->pVoice->Start(0);
    if (FAILED(hr)) {
        logInfo("BS: Start failed: hr=0x%08X\n", (unsigned)hr);
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

    if (xdkIsInstanceId(soundOrInstance)) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) destroyInstance(inst);
        return;
    }
    if (soundOrInstance < 0) return;
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance)
            destroyInstance(&arr->instances[i]);
    }
}

static void xdkStopAll(AudioSystem* audio) {
    XdkInstanceArray* arr = Instances((XdkAudioSystem*)audio);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active && arr->instances[i].soundIndex >= 0)
            destroyInstance(&arr->instances[i]);
    }
}

static bool xdkIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);

    if (xdkIsInstanceId(soundOrInstance)) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (!inst || inst->paused || !inst->pVoice) return false;
        XAUDIO2_VOICE_STATE state;
        inst->pVoice->GetState(&state, 0);
        return state.BuffersQueued > 0;
    }
    if (soundOrInstance < 0) return false;
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (!inst->active || inst->soundIndex != soundOrInstance ||
            inst->paused || !inst->pVoice) continue;
        XAUDIO2_VOICE_STATE state;
        inst->pVoice->GetState(&state, 0);
        if (state.BuffersQueued > 0) return true;
    }
    return false;
}

static void xdkPauseInstance(XdkSoundInstance* inst) {
    if (!inst || !inst->active || inst->paused) return;
    if (!inst->pVoice) {
        inst->paused = true;
        return;
    }
    HRESULT hr = inst->pVoice->Stop(0);
    if (FAILED(hr)) {
        logInfo("BS: XAudio2 pause failed: hr=0x%08X\n", (unsigned)hr);
        return;
    }
    inst->paused = true;
}

static void xdkResumeInstance(XdkSoundInstance* inst) {
    if (!inst || !inst->active || !inst->paused || !inst->pVoice) return;
    HRESULT hr = inst->pVoice->Start(0);
    if (FAILED(hr)) {
        logInfo("BS: XAudio2 resume failed: hr=0x%08X\n", (unsigned)hr);
        return;
    }
    inst->paused = false;
}

static void xdkPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    if (xdkIsInstanceId(soundOrInstance)) {
        xdkPauseInstance(findById(xa, soundOrInstance));
        return;
    }
    if (soundOrInstance < 0) return;
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance)
            xdkPauseInstance(&arr->instances[i]);
    }
}

static void xdkResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkInstanceArray* arr = Instances(xa);
    if (xdkIsInstanceId(soundOrInstance)) {
        xdkResumeInstance(findById(xa, soundOrInstance));
        return;
    }
    if (soundOrInstance < 0) return;
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance)
            xdkResumeInstance(&arr->instances[i]);
    }
}

static void xdkPauseAll(AudioSystem* audio) {
    XdkInstanceArray* arr = Instances((XdkAudioSystem*)audio);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active && arr->instances[i].soundIndex >= 0)
            xdkPauseInstance(&arr->instances[i]);
    }
}

static void xdkResumeAll(AudioSystem* audio) {
    XdkInstanceArray* arr = Instances((XdkAudioSystem*)audio);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active && arr->instances[i].soundIndex >= 0)
            xdkResumeInstance(&arr->instances[i]);
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
    XdkInstanceArray* arr = Instances(xa);
    if (soundOrInstance >= XDK_AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_INDEX_BASE;
        if (slot < 0 || slot >= XDK_MAX_AUDIO_STREAMS || !xa->streams[slot].active) return;
        xa->streams[slot].initialGain = gain;
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            XdkSoundInstance* inst = &arr->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance)
                xdkApplyGainTarget(xa, audio, inst, gain, timeMs);
        }
        return;
    }
    if (xdkIsInstanceId(soundOrInstance)) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) xdkApplyGainTarget(xa, audio, inst, gain, timeMs);
        return;
    }
    if (soundOrInstance < 0) return;
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance)
            xdkApplyGainTarget(xa, audio, inst, gain, timeMs);
    }
}

static float xdkGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (soundOrInstance >= XDK_AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_INDEX_BASE;
        return slot >= 0 && slot < XDK_MAX_AUDIO_STREAMS && xa->streams[slot].active
            ? xa->streams[slot].initialGain : 0.0f;
    }
    if (xdkIsInstanceId(soundOrInstance)) {
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
    if (!(pitch >= XAUDIO2_MIN_FREQ_RATIO && pitch <= XAUDIO2_MAX_FREQ_RATIO)) return;
    XdkInstanceArray* arr = Instances(xa);
    if (soundOrInstance >= XDK_AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_INDEX_BASE;
        if (slot < 0 || slot >= XDK_MAX_AUDIO_STREAMS || !xa->streams[slot].active) return;
        xa->streams[slot].initialPitch = pitch;
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            XdkSoundInstance* inst = &arr->instances[i];
            if (inst->active && inst->soundIndex == soundOrInstance)
                xdkApplyInstancePitch(inst, pitch);
        }
        return;
    }
    if (xdkIsInstanceId(soundOrInstance)) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) xdkApplyInstancePitch(inst, pitch);
        return;
    }
    if (soundOrInstance < 0) return;
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance)
            xdkApplyInstancePitch(inst, pitch);
    }
}

static float xdkGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (soundOrInstance >= XDK_AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_INDEX_BASE;
        return slot >= 0 && slot < XDK_MAX_AUDIO_STREAMS && xa->streams[slot].active
            ? xa->streams[slot].initialPitch : 1.0f;
    }
    if (xdkIsInstanceId(soundOrInstance)) {
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

static float xdkInstanceTrackPosition(XdkSoundInstance* inst) {
    if (!inst || !inst->pVoice || inst->sampleRate == 0 || inst->channels == 0) return 0.0f;
    XAUDIO2_VOICE_STATE state;
    inst->pVoice->GetState(&state, 0);
    uint64_t played = state.SamplesPlayed >= inst->samplesPlayedAtSeek
        ? state.SamplesPlayed - inst->samplesPlayedAtSeek : 0;
    uint64_t totalFrames = inst->pcmSize / ((uint32_t)inst->channels * sizeof(short));
    if (totalFrames == 0) return 0.0f;

    uint64_t position = (uint64_t)inst->positionOffsetSamples + played;
    if (inst->loop) {
        // After a seek, play the tail once, then loop the entire track. This
        // avoids accidentally turning a seek into a permanent tail-only loop.
        uint64_t firstPassLength = totalFrames - inst->positionOffsetSamples;
        if (played < firstPassLength)
            position = (uint64_t)inst->positionOffsetSamples + played;
        else
            position = (played - firstPassLength) % totalFrames;
    } else if (position > totalFrames) {
        position = totalFrames;
    }
    return (float)position / (float)inst->sampleRate;
}

static float xdkGetTrackPosition(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (xdkIsInstanceId(soundOrInstance))
        return xdkInstanceTrackPosition(findById(xa, soundOrInstance));

    if (soundOrInstance < 0) return 0.0f;
    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance)
            return xdkInstanceTrackPosition(inst);
    }
    return 0.0f;
}

static void xdkSeekInstance(XdkSoundInstance* inst, float positionSeconds) {
    if (!inst || !inst->pVoice || !inst->pcmData || inst->sampleRate == 0 ||
        inst->channels == 0 || !(positionSeconds >= 0.0f) || positionSeconds > FLT_MAX) return;

    uint32_t totalFrames = inst->pcmSize / ((uint32_t)inst->channels * sizeof(short));
    if (totalFrames == 0) return;

    // Use double precision for the seconds-to-frame conversion. A finite float
    // timestamp multiplied by sampleRate can overflow float before it is clamped.
    double requestedFrame = (double)positionSeconds * (double)inst->sampleRate;
    uint32_t offset;
    if (inst->loop) {
        offset = (uint32_t)fmod(requestedFrame, (double)totalFrames);
    } else {
        offset = requestedFrame >= (double)totalFrames
            ? totalFrames : (uint32_t)requestedFrame;
    }

    HRESULT hr = inst->pVoice->Stop(0);
    if (FAILED(hr)) return;
    hr = inst->pVoice->FlushSourceBuffers();
    if (FAILED(hr)) {
        logInfo("BS: seek FlushSourceBuffers failed: hr=0x%08X\n", (unsigned)hr);
        destroyInstance(inst);
        return;
    }

    XAUDIO2_VOICE_STATE state;
    inst->pVoice->GetState(&state, 0);
    inst->samplesPlayedAtSeek = state.SamplesPlayed;
    inst->positionOffsetSamples = offset;

    // A non-looping seek to/past EOF leaves the voice empty; the regular
    // update pass will retire it once it observes BuffersQueued == 0.
    if (offset >= totalFrames) return;

    if (inst->loop && offset > 0) {
        // Queue the remainder of the current play-through, followed by a
        // full-track infinite loop. XAudio2 loops within one buffer, so a
        // single buffer starting at offset would loop only the tail.
        XAUDIO2_BUFFER firstPass;
        memset(&firstPass, 0, sizeof(firstPass));
        firstPass.AudioBytes = inst->pcmSize;
        firstPass.pAudioData = inst->pcmData;
        firstPass.PlayBegin = offset;
        firstPass.PlayLength = totalFrames - offset;
        hr = inst->pVoice->SubmitSourceBuffer(&firstPass);
        if (FAILED(hr)) {
            logInfo("BS: seek first-pass SubmitSourceBuffer failed: hr=0x%08X\n", (unsigned)hr);
            destroyInstance(inst);
            return;
        }

        XAUDIO2_BUFFER loopBuffer;
        memset(&loopBuffer, 0, sizeof(loopBuffer));
        loopBuffer.AudioBytes = inst->pcmSize;
        loopBuffer.pAudioData = inst->pcmData;
        loopBuffer.PlayLength = totalFrames;
        loopBuffer.LoopLength = totalFrames;
        loopBuffer.LoopCount = XAUDIO2_LOOP_INFINITE;
        hr = inst->pVoice->SubmitSourceBuffer(&loopBuffer);
    } else {
        XAUDIO2_BUFFER buffer;
        memset(&buffer, 0, sizeof(buffer));
        buffer.Flags = inst->loop ? 0 : XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes = inst->pcmSize;
        buffer.pAudioData = inst->pcmData;
        buffer.PlayBegin = offset;
        buffer.PlayLength = totalFrames - offset;
        if (inst->loop) {
            buffer.LoopBegin = 0;
            buffer.LoopLength = totalFrames;
            buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
        }
        hr = inst->pVoice->SubmitSourceBuffer(&buffer);
    }
    if (FAILED(hr)) {
        logInfo("BS: seek SubmitSourceBuffer failed: hr=0x%08X\n", (unsigned)hr);
        destroyInstance(inst);
        return;
    }
    if (!inst->paused) {
        hr = inst->pVoice->Start(0);
        if (FAILED(hr)) {
            logInfo("BS: seek Start failed: hr=0x%08X\n", (unsigned)hr);
            destroyInstance(inst);
        }
    }
}

static void xdkSetTrackPosition(AudioSystem* audio, int32_t soundOrInstance, float positionSeconds) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!(positionSeconds >= 0.0f) || positionSeconds > FLT_MAX) return;

    if (xdkIsInstanceId(soundOrInstance)) {
        xdkSeekInstance(findById(xa, soundOrInstance), positionSeconds);
        return;
    }
    if (soundOrInstance < 0) return;

    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        XdkSoundInstance* inst = &arr->instances[i];
        if (inst->active && inst->soundIndex == soundOrInstance)
            xdkSeekInstance(inst, positionSeconds);
    }
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
        logInfo("BS: requested invalid audio group %d\n", groupIndex);
        return;
    }

    // Preserve sparse group indices with NULL entries. NULL means "not loaded"
    // and remains retryable; a parsed DataWin is the only loaded state.
    while (arrlen(audio->audioGroups) <= groupIndex) {
        DataWin* empty = NULL;
        arrput(audio->audioGroups, empty);
        if (arrlen(audio->audioGroups) <= groupIndex) {
            logInfo("BS: could not grow audio-group table for index %d\n", groupIndex);
            return;
        }
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
        logInfo("BS: audio group %d missing: %s\n", groupIndex, relativePath);
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
        logInfo("BS: audio group %d parse failed: %s\n", groupIndex, relativePath);
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
    // Platform suspension must also stop FMOD/encoded voices, unlike the
    // GameMaker audio_pause_all operation which intentionally excludes them.
    XdkInstanceArray* arr = Instances((XdkAudioSystem*)audio);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++)
        xdkPauseInstance(&arr->instances[i]);
}

static void xdkResume(AudioSystem* audio) {
    XdkInstanceArray* arr = Instances((XdkAudioSystem*)audio);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++)
        xdkResumeInstance(&arr->instances[i]);
}

static bool xdkReadExternalAudioForLength(XdkAudioSystem* xa, const char* file,
                                           uint8_t** outData, uint32_t* outBytes) {
    if (!xa || !xa->fileSystem || !file || !file[0] || !outData || !outBytes) return false;
    *outData = NULL;
    *outBytes = 0;

    const char* fileName = file;
    const char* slash = strrchr(file, '/');
    const char* backslash = strrchr(file, '\\');
    if (slash && slash + 1 > fileName) fileName = slash + 1;
    if (backslash && backslash + 1 > fileName) fileName = backslash + 1;
    bool hasExt = strchr(fileName, '.') != NULL;
    size_t fileLen = strlen(file);
    size_t extensionLen = hasExt ? 0 : 4;
    if (fileLen + extensionLen >= 512) return false;

    char filename[512];
    memcpy(filename, file, fileLen);
    if (!hasExt) memcpy(filename + fileLen, ".ogg", 5);
    else filename[fileLen] = '\0';

    FileSystem* fs = xa->fileSystem;
    void* handle = fs->vtable->binaryOpen(fs, filename, GML_FILE_BIN_READ);
    if (!handle) return false;

    int32_t size = fs->vtable->binarySize(fs, handle);
    if (size <= 0 || (uint32_t)size > XDK_MAX_ENCODED_AUDIO_BYTES) {
        fs->vtable->binaryClose(fs, handle);
        return false;
    }
    uint8_t* data = (uint8_t*)malloc((size_t)size);
    if (!data) {
        fs->vtable->binaryClose(fs, handle);
        return false;
    }

    int32_t totalRead = 0;
    while (totalRead < size) {
        int32_t remaining = size - totalRead;
        int32_t got = fs->vtable->binaryRead(fs, handle, data + totalRead, remaining);
        if (got <= 0 || got > remaining) break;
        totalRead += got;
    }
    fs->vtable->binaryClose(fs, handle);
    if (totalRead != size) {
        free(data);
        return false;
    }
    *outData = data;
    *outBytes = (uint32_t)size;
    return true;
}

static float xdkMeasureAudioBytes(const uint8_t* data, uint32_t bytes) {
    if (!data || bytes == 0 || bytes > XDK_MAX_ENCODED_AUDIO_BYTES) return 0.0f;

    if (bytes >= 12 && memcmp(data, "RIFF", 4) == 0) {
        uint32_t riffBytes = xdkReadLE32(data + 4);
        if (memcmp(data + 8, "WAVE", 4) != 0 || riffBytes < 4 ||
            riffBytes > bytes - 8) return 0.0f;

        size_t offset = 12;
        size_t riffEnd = (size_t)riffBytes + 8;
        uint16_t formatTag = 0, channels = 0, blockAlign = 0, bitsPerSample = 0;
        uint32_t sampleRate = 0, dataSize = 0;
        bool haveFmt = false, haveData = false;
        while (offset <= riffEnd && riffEnd - offset >= 8) {
            const uint8_t* chunk = data + offset;
            uint32_t chunkBytes = xdkReadLE32(chunk + 4);
            size_t payload = offset + 8;
            if ((size_t)chunkBytes > riffEnd - payload) return 0.0f;

            if (memcmp(chunk, "fmt ", 4) == 0) {
                if (haveFmt || chunkBytes < 16) return 0.0f;
                formatTag = xdkReadLE16(data + payload);
                channels = xdkReadLE16(data + payload + 2);
                sampleRate = xdkReadLE32(data + payload + 4);
                blockAlign = xdkReadLE16(data + payload + 12);
                bitsPerSample = xdkReadLE16(data + payload + 14);
                haveFmt = true;
            } else if (memcmp(chunk, "data", 4) == 0) {
                if (haveData) return 0.0f;
                dataSize = chunkBytes;
                haveData = true;
            }

            size_t padded = (size_t)chunkBytes + (chunkBytes & 1U);
            if (padded > riffEnd - payload) return 0.0f;
            offset = payload + padded;
        }
        if (offset != riffEnd || !haveFmt || !haveData || formatTag != 1 ||
            bitsPerSample != 16 || channels == 0 || channels > 2 ||
            sampleRate == 0 || sampleRate > XDK_MAX_AUDIO_SAMPLE_RATE || blockAlign != channels * 2 ||
            dataSize == 0 || dataSize % blockAlign != 0) return 0.0f;
        return (float)(dataSize / blockAlign) / (float)sampleRate;
    }

    int error = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(data, (int)bytes, &error, NULL);
    if (!vorbis) return 0.0f;
    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    unsigned int frames = stb_vorbis_stream_length_in_samples(vorbis);
    float duration = info.sample_rate > 0 && info.sample_rate <= XDK_MAX_AUDIO_SAMPLE_RATE && frames > 0
        ? (float)frames / (float)info.sample_rate : 0.0f;
    stb_vorbis_close(vorbis);
    return duration;
}

static float xdkGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;

    if (soundOrInstance >= XDK_AUDIO_STREAM_INDEX_BASE) {
        int32_t slot = soundOrInstance - XDK_AUDIO_STREAM_INDEX_BASE;
        if (slot < 0 || slot >= XDK_MAX_AUDIO_STREAMS || !xa->streams[slot].active)
            return 0.0f;
        XdkAudioStream* stream = &xa->streams[slot];
        if (stream->lengthSeconds >= 0.0f) return stream->lengthSeconds;

        uint8_t* encoded = NULL;
        uint32_t encodedBytes = 0;
        if (!xdkReadExternalAudioForLength(xa, stream->filePath, &encoded, &encodedBytes)) {
            stream->lengthSeconds = 0.0f;
            return 0.0f;
        }
        stream->lengthSeconds = xdkMeasureAudioBytes(encoded, encodedBytes);
        free(encoded);
        return stream->lengthSeconds;
    }

    XdkSoundInstance* inst = xdkIsInstanceId(soundOrInstance)
        ? findById(xa, soundOrInstance) : NULL;
    if (xdkIsInstanceId(soundOrInstance)) {
        return inst && inst->sampleRate > 0 && inst->channels > 0
            ? (float)inst->pcmSize / ((float)inst->sampleRate * (float)inst->channels * 2.0f)
            : 0.0f;
    }

    if (soundOrInstance < 0 || !audio->dw ||
        (uint32_t)soundOrInstance >= audio->dw->sond.count) return 0.0f;

    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance) {
            inst = &arr->instances[i];
            if (inst->sampleRate > 0 && inst->channels > 0)
                return (float)inst->pcmSize / ((float)inst->sampleRate * (float)inst->channels * 2.0f);
        }
    }

    // A valid SOND length query works before playback.
    Sound* sound = &audio->dw->sond.sounds[soundOrInstance];
    int32_t groupIndex = sound->audioGroup < 0 ? 0 : sound->audioGroup;
    if (groupIndex > 0 && !xdkGroupIsLoaded(audio, groupIndex))
        xdkGroupLoad(audio, groupIndex);
    if (groupIndex < 0 || (uint32_t)groupIndex >= arrlen(audio->audioGroups) ||
        !audio->audioGroups[groupIndex]) return 0.0f;
    DataWin* audioData = audio->audioGroups[groupIndex];

    bool isRegular = (sound->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR;
    bool isEmbedded = (sound->flags & AUDIO_ENTRY_FLAG_IS_EMBEDDED) != 0;
    bool isCompressed = (sound->flags & AUDIO_ENTRY_FLAG_IS_COMPRESSED) != 0;
    bool inAudo = !isRegular || isEmbedded || isCompressed;

    const uint8_t* encoded = NULL;
    uint8_t* owned = NULL;
    uint32_t encodedBytes = 0;
    if (inAudo) {
        if (sound->audioFile < 0 || (uint32_t)sound->audioFile >= audioData->audo.count) return 0.0f;
        DataWin_loadAudoIfNeeded(audioData, (uint32_t)sound->audioFile);
        AudioEntry* entry = &audioData->audo.entries[sound->audioFile];
        if (!entry->data || entry->dataSize == 0 ||
            entry->dataSize > XDK_MAX_ENCODED_AUDIO_BYTES) return 0.0f;
        encoded = entry->data;
        encodedBytes = entry->dataSize;
    } else {
        if (!xdkReadExternalAudioForLength(xa, sound->file, &owned, &encodedBytes))
            return 0.0f;
        encoded = owned;
    }

    float duration = xdkMeasureAudioBytes(encoded, encodedBytes);
    free(owned);
    return duration;
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
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!xa || !xa->initialized || !xa->fileSystem || !filename || !filename[0]) return -1;

    // Keep stream creation cheap: resolve and retain the path. The current
    // playback backend decodes the file into bounded PCM when playSound runs.
    int32_t slot = -1;
    for (int32_t i = 0; i < XDK_MAX_AUDIO_STREAMS; i++) {
        if (!xa->streams[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    char* resolved = xa->fileSystem->vtable->resolvePath(xa->fileSystem, filename);
    if (!resolved) {
        logInfo("BS: could not resolve stream path '%s'\n", filename);
        return -1;
    }

    XdkAudioStream* stream = &xa->streams[slot];
    stream->active = true;
    stream->filePath = resolved;
    stream->initialGain = 1.0f;
    stream->initialPitch = 1.0f;
    stream->lengthSeconds = -1.0f;
    return XDK_AUDIO_STREAM_INDEX_BASE + slot;
}

static bool xdkDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!xa || streamIndex < XDK_AUDIO_STREAM_INDEX_BASE ||
        streamIndex >= XDK_AUDIO_STREAM_INDEX_BASE + XDK_MAX_AUDIO_STREAMS)
        return false;
    int32_t slot = streamIndex - XDK_AUDIO_STREAM_INDEX_BASE;
    if (!xa->streams[slot].active) return false;

    XdkInstanceArray* arr = Instances(xa);
    for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
        if (arr->instances[i].active && arr->instances[i].soundIndex == streamIndex)
            destroyInstance(&arr->instances[i]);
    }

    free(xa->streams[slot].filePath);
    memset(&xa->streams[slot], 0, sizeof(xa->streams[slot]));
    return true;
}

static int32_t xdkPlayEncoded(AudioSystem* audio, const uint8_t* data, size_t bytes, bool loop) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (!xa || !xa->initialized || !data || bytes == 0 ||
        bytes > XDK_MAX_ENCODED_AUDIO_BYTES) return -1;

    int stbErr = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(data, (int)bytes, &stbErr, NULL);
    if (!vorbis) {
        logInfo("BS: playEncoded vorbis open failed: err=%d size=%u\n", stbErr, (unsigned)bytes);
        return -1;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    unsigned int totalSamples = stb_vorbis_stream_length_in_samples(vorbis);
    if (info.channels < 1 || info.channels > 2 || info.sample_rate == 0 || info.sample_rate > XDK_MAX_AUDIO_SAMPLE_RATE ||
        totalSamples == 0 || totalSamples > (unsigned int)(0x7fffffff / (info.channels * (int)sizeof(short)))) {
        stb_vorbis_close(vorbis);
        logInfo("BS: playEncoded unsupported stream metadata\n");
        return -1;
    }

    size_t pcmCapacity = (size_t)totalSamples * (size_t)info.channels * sizeof(short);
    if (!xdkCanAllocatePcm(xa, pcmCapacity)) {
        stb_vorbis_close(vorbis);
        logInfo("BS: encoded PCM exceeds active memory budget before allocation\n");
        return -1;
    }
    short* pcm = (short*)malloc(pcmCapacity);
    if (!pcm) {
        stb_vorbis_close(vorbis);
        return -1;
    }
    int sampleCount = stb_vorbis_get_samples_short_interleaved(
        vorbis, info.channels, pcm, (int)(totalSamples * (unsigned int)info.channels));
    stb_vorbis_close(vorbis);
    if (sampleCount <= 0) {
        free(pcm);
        return -1;
    }

    size_t pcmBytes = (size_t)sampleCount * (size_t)info.channels * sizeof(short);
    if (pcmBytes == 0 || pcmBytes > 0x7fffffffU) {
        free(pcm);
        return -1;
    }

    XdkSoundInstance* inst = findFreeSlot(xa);
    if (!inst) {
        free(pcm);
        return -1;
    }
    int32_t instanceId = allocateInstanceId(xa);
    if (instanceId < 0) {
        free(pcm);
        return -1;
    }

    memset(inst, 0, sizeof(*inst));
    inst->active = true;
    inst->soundIndex = -1; // Encoded FMOD voices are not tied to a SOND entry.
    inst->instanceId = instanceId;
    inst->loop = loop;
    inst->pitch = 1.0f;
    inst->sondPitch = 1.0f;
    inst->sondVolume = 1.0f;
    inst->currentGain = inst->targetGain = inst->startGain = 1.0f;
    if (pcmBytes > xa->maxPcmBytes ||
        xa->activePcmBytes > xa->maxPcmBytes - (uint32_t)pcmBytes) {
        logInfo("BS: encoded PCM exceeds active memory budget (requested=%u active=%u limit=%u)\n",
                 (unsigned)pcmBytes, xa->activePcmBytes, xa->maxPcmBytes);
        free(pcm);
        destroyInstance(inst);
        return -1;
    }

    inst->sampleRate = info.sample_rate;
    inst->channels = (uint16_t)info.channels;
    inst->pcmData = (uint8_t*)pcm;
    inst->pcmSize = (uint32_t)pcmBytes;
    inst->owner = xa;
    xa->activePcmBytes += (uint32_t)pcmBytes;

    WAVEFORMATEXTENSIBLE wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = (WORD)info.channels;
    wfx.Format.nSamplesPerSec = info.sample_rate;
    wfx.Format.wBitsPerSample = 16;
    wfx.Format.nBlockAlign = (WORD)(info.channels * 2);
    wfx.Format.nAvgBytesPerSec = info.sample_rate * info.channels * 2;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 16;
    wfx.dwChannelMask = info.channels == 1
        ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    IXAudio2* pXA = (IXAudio2*)xa->pXAudio2;
    HRESULT hr = pXA->CreateSourceVoice(&inst->pVoice, (WAVEFORMATEX*)&wfx, 0, XAUDIO2_MAX_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr) || !inst->pVoice) {
        logInfo("BS: playEncoded CreateSourceVoice failed: hr=0x%08X\n", (unsigned)hr);
        destroyInstance(inst);
        return -1;
    }

    XAUDIO2_BUFFER buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.Flags = loop ? 0 : XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes = inst->pcmSize;
    buffer.pAudioData = inst->pcmData;
    if (loop) buffer.LoopCount = XAUDIO2_LOOP_INFINITE;

    hr = inst->pVoice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr)) {
        logInfo("BS: playEncoded SubmitSourceBuffer failed: hr=0x%08X\n", (unsigned)hr);
        destroyInstance(inst);
        return -1;
    }
    hr = inst->pVoice->SetVolume(xa->masterGain);
    if (FAILED(hr) || !xdkApplyInstancePitch(inst, 1.0f)) {
        logInfo("BS: playEncoded voice state setup failed: hr=0x%08X\n", (unsigned)hr);
        destroyInstance(inst);
        return -1;
    }
    hr = inst->pVoice->Start(0);
    if (FAILED(hr)) {
        logInfo("BS: playEncoded Start failed: hr=0x%08X\n", (unsigned)hr);
        destroyInstance(inst);
        return -1;
    }

    logInfo("BS: playEncoded started id=%d ch=%d rate=%u samples=%d loop=%d\n",
             instanceId, info.channels, info.sample_rate, sampleCount, loop ? 1 : 0);
    return instanceId;
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
