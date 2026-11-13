#include <xtl.h>
#include <xaudio2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// stb_vorbis on big-endian Xbox 360:
// Disable the fast float-to-int path which has endian assumptions.
// Use the slow but portable path instead.
#define STB_VORBIS_NO_FAST_SCALED_FLOAT
#include "stb_vorbis.c"

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "utils.h"
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
    int32_t idx = id - XDK_SOUND_INSTANCE_ID_BASE;
    if (idx < 0 || idx >= XDK_MAX_SOUND_INSTANCES) return NULL;
    XdkSoundInstance* inst = &Instances(xa)->instances[idx];
    if (!inst->active || inst->instanceId != id) return NULL;
    return inst;
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

    int32_t slotIndex = (int32_t)(inst - Instances(xa)->instances);
    inst->instanceId = XDK_SOUND_INSTANCE_ID_BASE + slotIndex;
    xa->nextInstanceCounter++;

    Sound* sound = &dw->sond.sounds[soundIndex];
    DbgPrint("BS: playSound idx=%d name=%s flags=0x%x audioFile=%d\n",
        soundIndex, sound->name ? sound->name : "?", (unsigned)sound->flags, sound->audioFile);
    if (sound->volume >= 0) {
        inst->sondVolume = sound->volume;
    }

    // Decode OGG audio — either embedded (AUDO) or external (.ogg file)
    bool isEmbedded = (sound->flags & 0x01) != 0;

    uint8_t* oggData = NULL;
    int oggSize = 0;
    bool freeOggData = false;

    if (isEmbedded) {
        if (sound->audioFile < 0 || (uint32_t)sound->audioFile >= dw->audo.count) {
            DbgPrint("BS: bad audioFile idx %d (audo count=%d)\n", sound->audioFile, (int)dw->audo.count);
            return inst->instanceId;
        }
        AudioEntry* entry = &dw->audo.entries[sound->audioFile];
        if (!entry->data || entry->dataSize == 0) {
            DbgPrint("BS: empty audio entry\n");
            return inst->instanceId;
        }
        oggData = entry->data;
        oggSize = (int)entry->dataSize;
    } else {
        // External: load .ogg file via FileSystem
        const char* file = sound->file;
        if (!file || !file[0] || !xa->fileSystem) {
            DbgPrint("BS: external sound: no file path or no filesystem\n");
            return inst->instanceId;
        }
        bool hasExt = (strchr(file, '.') != NULL);
        char filename[512];
        if (hasExt) snprintf(filename, sizeof(filename), "%s", file);
        else snprintf(filename, sizeof(filename), "%s.ogg", file);

        char* fullPath = xa->fileSystem->vtable->resolvePath(xa->fileSystem, filename);
        if (!fullPath) { DbgPrint("BS: could not resolve path for %s\n", filename); return inst->instanceId; }
        DbgPrint("BS: loading external audio: %s\n", fullPath);
        FILE* f = fopen(fullPath, "rb");
        free(fullPath);
        if (!f) { DbgPrint("BS: fopen failed for external audio\n"); return inst->instanceId; }
        fseek(f, 0, SEEK_END);
        oggSize = (int)ftell(f);
        fseek(f, 0, SEEK_SET);
        if (oggSize <= 0) { fclose(f); return inst->instanceId; }
        oggData = (uint8_t*)malloc(oggSize);
        fread(oggData, 1, oggSize, f);
        fclose(f);
        freeOggData = true;
    }

    // Detect format from header bytes
    int channels = 0, sampleRate = 0, sampleCount = 0;
    short* pcm = NULL;

    // Check if it's WAV (RIFF header) or OGG (OggS header)
    if (oggSize >= 44 && oggData[0] == 'R' && oggData[1] == 'I' && oggData[2] == 'F' && oggData[3] == 'F') {
        // WAV format — parse RIFF/WAVE header
        // Bytes 22-23: channels (LE uint16)
        // Bytes 24-27: sample rate (LE uint32)
        // Bytes 34-35: bits per sample (LE uint16)
        channels = (int)(oggData[22] | (oggData[23] << 8));
        sampleRate = (int)(oggData[24] | (oggData[25] << 8) | (oggData[26] << 16) | (oggData[27] << 24));
        int bitsPerSample = (int)(oggData[34] | (oggData[35] << 8));

        // Find "data" subchunk
        int dataOffset = 12;
        int dataSize = 0;
        while (dataOffset + 8 <= oggSize) {
            if (oggData[dataOffset] == 'd' && oggData[dataOffset+1] == 'a' &&
                oggData[dataOffset+2] == 't' && oggData[dataOffset+3] == 'a') {
                dataSize = (int)(oggData[dataOffset+4] | (oggData[dataOffset+5] << 8) |
                                 (oggData[dataOffset+6] << 16) | (oggData[dataOffset+7] << 24));
                dataOffset += 8;
                break;
            }
            int chunkSize = (int)(oggData[dataOffset+4] | (oggData[dataOffset+5] << 8) |
                                  (oggData[dataOffset+6] << 16) | (oggData[dataOffset+7] << 24));
            dataOffset += 8 + chunkSize;
        }

        if (dataSize > 0 && bitsPerSample == 16 && channels > 0) {
            sampleCount = dataSize / (channels * 2);
            pcm = (short*)malloc(dataSize);
            memcpy(pcm, oggData + dataOffset, dataSize);
            // WAV stores little-endian PCM; Xbox 360 XAudio2 expects big-endian
            for (int s = 0; s < sampleCount * channels; s++) {
                uint16_t v = (uint16_t)pcm[s];
                pcm[s] = (short)((v >> 8) | (v << 8));
            }
            DbgPrint("BS: WAV: ch=%d rate=%d samples=%d\n", channels, sampleRate, sampleCount);
        } else {
            DbgPrint("BS: WAV parse failed: bits=%d ch=%d dataSize=%d\n", bitsPerSample, channels, dataSize);
            if (freeOggData) free(oggData);
            return inst->instanceId;
        }
    } else {
        // Try OGG/Vorbis
        int stbErr = 0;
        stb_vorbis* vorbis = stb_vorbis_open_memory(oggData, oggSize, &stbErr, NULL);
        if (!vorbis) {
            DbgPrint("BS: vorbis open failed: err=%d size=%d\n", stbErr, oggSize);
            if (freeOggData) free(oggData);
            return inst->instanceId;
        }
        stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        channels = info.channels;
        sampleRate = info.sample_rate;
        int totalSamples = stb_vorbis_stream_length_in_samples(vorbis);
        pcm = (short*)malloc(totalSamples * channels * sizeof(short));
        sampleCount = stb_vorbis_get_samples_short_interleaved(vorbis, channels, pcm, totalSamples * channels);
        stb_vorbis_close(vorbis);
        DbgPrint("BS: OGG: ch=%d rate=%d samples=%d\n", channels, sampleRate, sampleCount);
    }

    if (freeOggData) free(oggData);

    if (sampleCount <= 0 || !pcm) {
        free(pcm);
        return inst->instanceId;
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
        free(inst->pcmData);
        inst->pcmData = NULL;
        return inst->instanceId;
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
    }
    inst->pVoice->SetVolume(inst->currentGain * inst->sondVolume * xa->masterGain);
    hr = inst->pVoice->Start(0);
    if (FAILED(hr)) {
        DbgPrint("BS: Start failed: hr=0x%08X\n", (unsigned)hr);
    }

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

static void xdkSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    XdkSoundInstance* inst = NULL;

    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        inst = findById(xa, soundOrInstance);
    } else {
        XdkInstanceArray* arr = Instances(xa);
        for (int i = 0; i < XDK_MAX_SOUND_INSTANCES; i++) {
            if (arr->instances[i].active && arr->instances[i].soundIndex == soundOrInstance) {
                inst = &arr->instances[i]; break;
            }
        }
    }
    if (!inst) return;

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

static float xdkGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        return inst ? inst->currentGain : 0.0f;
    }
    return 0.0f;
}

static void xdkSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        if (inst) {
            inst->pitch = pitch;
            if (inst->pVoice) inst->pVoice->SetFrequencyRatio(pitch * inst->sondPitch);
        }
    }
}

static float xdkGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    XdkAudioSystem* xa = (XdkAudioSystem*)audio;
    if (soundOrInstance >= XDK_SOUND_INSTANCE_ID_BASE) {
        XdkSoundInstance* inst = findById(xa, soundOrInstance);
        return inst ? inst->pitch : 1.0f;
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
        DbgPrint("BS: requested invalid audio group %d\\n", groupIndex);
        return;
    }
    while (arrlen(audio->audioGroups) <= groupIndex) {
        int32_t nextIndex = (int32_t)arrlen(audio->audioGroups);
        if (nextIndex != groupIndex) {
            DataWin* placeholder = (DataWin*)calloc(1, sizeof(DataWin));
            arrput(audio->audioGroups, placeholder);
            continue;
        }

        AudioGroup* group = &audio->dw->agrp.audioGroups[groupIndex];
        char defaultPath[64];
        const char* relativePath = group->path;
        if (!relativePath || !relativePath[0]) {
            sprintf(defaultPath, "audiogroup%d.dat", groupIndex);
            relativePath = defaultPath;
        }

        FileSystem* fs = ((XdkAudioSystem*)audio)->fileSystem;
        if (!fs || !fs->vtable->fileExists(fs, relativePath)) {
            DbgPrint("BS: audio group %d missing: %s\\n", groupIndex, relativePath);
            DataWin* placeholder = (DataWin*)calloc(1, sizeof(DataWin));
            arrput(audio->audioGroups, placeholder);
            break;
        }

        char* fullPath = fs->vtable->resolvePath(fs, relativePath);
        if (!fullPath) {
            DataWin* placeholder = (DataWin*)calloc(1, sizeof(DataWin));
            arrput(audio->audioGroups, placeholder);
            break;
        }

        DataWinParserOptions options;
        memset(&options, 0, sizeof(options));
        options.parseAudo = true;
        options.lazyLoadAudio = audio->dw->lazyLoadAudio;
        if (audio->dw->mappedFile) options.loadType = DATAWINLOADTYPE_MAP_FILE;
        DataWin* groupData = DataWin_parse(fullPath, options);
        free(fullPath);
        if (!groupData) groupData = (DataWin*)calloc(1, sizeof(DataWin));
        arrput(audio->audioGroups, groupData);
    }
}

static bool xdkGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    return groupIndex >= 0 && arrlen(audio->audioGroups) > groupIndex;
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
    xdkInitializeAudioVtable();
    xa->base.vtable = &xdkAudioVtable;
    xa->masterGain = 1.0f;
    xa->instanceData = calloc(1, sizeof(XdkInstanceArray));
    return xa;
}
