#include "AudioManager.h"
#include <SDL.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// One process-wide instance. Lives until program end; SDL is shut down by
// shutdown() (called from Game::shutdown).
static AudioManager g_audio;
AudioManager& gAudio() { return g_audio; }

struct AudioManager::Impl {
    SDL_AudioDeviceID dev = 0;
    SDL_AudioSpec     spec{};                       // device's actual spec
    struct Clip { std::vector<Uint8> data; };
    std::unordered_map<std::string, Clip>     clips;
    std::unordered_set<std::string>           warned;
    // Per-key cooldown to suppress duplicate SFX when both the explicit
    // call site (button click) and the field-state observer fire the same
    // key in the same frame. SDL_GetTicks() returns ms since SDL init.
    std::unordered_map<std::string, Uint32>   lastPlayMs;
    Uint32 cooldownMs = 90;
    bool   muted  = false;
    float  volume = 0.7f;
    bool   ready  = false;
};

// The canonical SFX bank — tools/generate_sfx.py produces exactly these and
// Game.cpp loads exactly these. Sharing the list here keeps the diagnostic
// count consistent across the whole app.
static const char* const kSfxBank[] = {
    "click", "hover", "confirm", "cancel", "error",
    "draw", "shuffle", "summon", "special_summon", "set",
    "activate", "chain", "send_gy", "banish", "attack",
    "damage", "victory", "defeat", "duel_start",
    nullptr
};
const char* const* AudioManager::expectedSfx() { return kSfxBank; }
int AudioManager::expectedSfxCount() {
    int n = 0; while (kSfxBank[n]) ++n; return n;
}

AudioManager::AudioManager()  : p(new Impl()) {}
AudioManager::~AudioManager() { shutdown(); delete p; p = nullptr; }
bool AudioManager::isAvailable() const { return p && p->ready; }

bool AudioManager::init() {
    if (p->ready) return true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        printf("[audio] SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{};
    want.freq     = 44100;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 2048;
    want.callback = nullptr;                        // queue mode
    p->dev = SDL_OpenAudioDevice(nullptr, 0, &want, &p->spec,
                                 SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!p->dev) {
        printf("[audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_PauseAudioDevice(p->dev, 0);                // start playback
    p->ready = true;
    printf("[audio] device opened: %d Hz, %d ch, fmt 0x%x\n",
           p->spec.freq, p->spec.channels, (unsigned)p->spec.format);
    return true;
}

void AudioManager::shutdown() {
    if (!p) return;
    if (p->dev) {
        SDL_ClearQueuedAudio(p->dev);
        SDL_CloseAudioDevice(p->dev);
        p->dev = 0;
    }
    if (p->ready) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        p->ready = false;
    }
    p->clips.clear();
    p->warned.clear();
}

void AudioManager::load(const std::string& key, const std::string& path) {
    if (!p->ready) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        if (p->warned.insert(path).second)
            printf("[audio] missing SFX (ok, will play silent): %s\n",
                   path.c_str());
        return;
    }
    SDL_AudioSpec wavSpec{};
    Uint8* wavBuf = nullptr;
    Uint32 wavLen = 0;
    if (!SDL_LoadWAV(path.c_str(), &wavSpec, &wavBuf, &wavLen)) {
        if (p->warned.insert(path).second)
            printf("[audio] SDL_LoadWAV failed for %s: %s\n",
                   path.c_str(), SDL_GetError());
        return;
    }
    // Build a converter from the WAV's spec to the device's spec.
    SDL_AudioCVT cvt{};
    int rc = SDL_BuildAudioCVT(&cvt,
                               wavSpec.format,   wavSpec.channels,   wavSpec.freq,
                               p->spec.format,   p->spec.channels,   p->spec.freq);
    if (rc < 0) {
        if (p->warned.insert(path).second)
            printf("[audio] SDL_BuildAudioCVT failed for %s: %s\n",
                   path.c_str(), SDL_GetError());
        SDL_FreeWAV(wavBuf);
        return;
    }
    Impl::Clip clip;
    if (rc == 0) {
        // No conversion needed — just copy the raw WAV bytes.
        clip.data.assign(wavBuf, wavBuf + wavLen);
    } else {
        // Conversion needed. SDL needs len_mult * src_len bytes of scratch.
        cvt.len = (int)wavLen;
        std::vector<Uint8> buf(cvt.len * cvt.len_mult, 0);
        cvt.buf = buf.data();
        std::copy(wavBuf, wavBuf + wavLen, buf.begin());
        if (SDL_ConvertAudio(&cvt) < 0) {
            if (p->warned.insert(path).second)
                printf("[audio] SDL_ConvertAudio failed for %s: %s\n",
                       path.c_str(), SDL_GetError());
            SDL_FreeWAV(wavBuf);
            return;
        }
        clip.data.assign(buf.begin(), buf.begin() + cvt.len_cvt);
    }
    SDL_FreeWAV(wavBuf);
    p->clips[key] = std::move(clip);
}

void AudioManager::play(const std::string& key) {
    if (!p->ready || p->muted || p->volume < 0.001f) return;
    auto it = p->clips.find(key);
    if (it == p->clips.end()) return;
    const auto& clip = it->second;
    if (clip.data.empty()) return;
    // Cooldown — suppress duplicate plays of the SAME key within the
    // cooldown window. Stops the action-popup button and the field-state
    // observer from doubling up on a Normal Summon, etc.
    Uint32 now = SDL_GetTicks();
    auto lp = p->lastPlayMs.find(key);
    if (lp != p->lastPlayMs.end() && now - lp->second < p->cooldownMs)
        return;
    p->lastPlayMs[key] = now;
    if (p->volume >= 0.995f) {
        SDL_QueueAudio(p->dev, clip.data.data(),
                       (Uint32)clip.data.size());
    } else {
        // Apply volume via SDL_MixAudioFormat into a temporary buffer.
        std::vector<Uint8> tmp(clip.data.size(), 0);
        int sdlVol = (int)(SDL_MIX_MAXVOLUME * p->volume);
        SDL_MixAudioFormat(tmp.data(), clip.data.data(), p->spec.format,
                           (Uint32)clip.data.size(), sdlVol);
        SDL_QueueAudio(p->dev, tmp.data(), (Uint32)tmp.size());
    }
}

void  AudioManager::setMuted(bool m)     { p->muted = m; }
bool  AudioManager::muted() const        { return p->muted; }
void  AudioManager::setVolume(float v)   { p->volume = std::clamp(v, 0.f, 1.f); }
float AudioManager::volume() const       { return p->volume; }

bool AudioManager::isLoaded(const std::string& key) const {
    return p && p->clips.find(key) != p->clips.end();
}
int AudioManager::loadedCount() const {
    return p ? (int)p->clips.size() : 0;
}
