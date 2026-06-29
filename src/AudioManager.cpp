#include "AudioManager.h"
#include <SDL.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    float  masterVolume = 1.0f;       // top-of-mixer gain (SFX + music)
    bool   muteUiSfx = false;         // drop hover/draw ambience
    bool   ready  = false;

    // ── Streaming music (separate callback device) ────────────────────────
    SDL_AudioDeviceID musicDev = 0;
    SDL_AudioSpec     musicSpec{};
    Clip              music;          // PCM converted to musicSpec
    size_t            musicPos = 0;   // read cursor (audio thread)
    bool              musicOn  = false;
    float             musicVolume = 0.5f;
};

// Audio-thread callback: fill `stream` with the looping music PCM, mixed at
// the music volume. Runs on SDL's audio thread, so it only touches fields the
// main thread guards with SDL_LockAudioDevice when mutating the buffer.
void AudioManager::musicCallback(void* userdata, unsigned char* stream, int len) {
    auto* p = static_cast<AudioManager::Impl*>(userdata);
    SDL_memset(stream, 0, (size_t)len);
    if (!p->musicOn || p->muted || p->music.data.empty() ||
        p->musicVolume < 0.001f)
        return;
    const std::vector<Uint8>& d = p->music.data;
    int vol = (int)(SDL_MIX_MAXVOLUME * p->musicVolume * p->masterVolume);
    int filled = 0;
    while (filled < len) {
        if (p->musicPos >= d.size()) p->musicPos = 0;          // loop
        int chunk = (int)std::min((size_t)(len - filled),
                                  d.size() - p->musicPos);
        SDL_MixAudioFormat(stream + filled, d.data() + p->musicPos,
                           p->musicSpec.format, (Uint32)chunk, vol);
        p->musicPos += (size_t)chunk;
        filled      += chunk;
    }
}

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

    // Second device for streaming music (callback mode), so BGM mixes UNDER
    // the queued SFX rather than playing in sequence with them.
    SDL_AudioSpec mwant{};
    mwant.freq     = 44100;
    mwant.format   = AUDIO_S16SYS;
    mwant.channels = 2;
    mwant.samples  = 4096;
    mwant.callback = &AudioManager::musicCallback;
    mwant.userdata = p;
    p->musicDev = SDL_OpenAudioDevice(nullptr, 0, &mwant, &p->musicSpec,
                                      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (p->musicDev) SDL_PauseAudioDevice(p->musicDev, 1);   // paused until play
    else printf("[audio] music device failed (BGM disabled): %s\n",
                SDL_GetError());
    return true;
}

void AudioManager::shutdown() {
    if (!p) return;
    if (p->musicDev) {
        SDL_CloseAudioDevice(p->musicDev);
        p->musicDev = 0;
    }
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
    float eff = p->volume * p->masterVolume;
    if (!p->ready || p->muted || eff < 0.001f) return;
    // Ambient UI sounds the player can silence independently.
    if (p->muteUiSfx && (key == "hover" || key == "draw")) return;
    auto it = p->clips.find(key);
    if (it == p->clips.end()) return;
    const auto& clip = it->second;
    if (clip.data.empty()) return;
    // Single channel: never stack SFX. If a clip is still playing, drop this
    // new one instead of queueing it to play afterwards. SFX fire in bursts
    // (summon + send-to-GY + chain in one beat) and the old queue model made
    // them play back-to-back, lagging seconds behind the action. With this
    // gate the in-progress sound finishes and the extras are simply discarded.
    if (SDL_GetQueuedAudioSize(p->dev) > 0) return;
    // Cooldown — suppress duplicate plays of the SAME key within the
    // cooldown window. Stops the action-popup button and the field-state
    // observer from doubling up on a Normal Summon, etc.
    Uint32 now = SDL_GetTicks();
    auto lp = p->lastPlayMs.find(key);
    if (lp != p->lastPlayMs.end() && now - lp->second < p->cooldownMs)
        return;
    p->lastPlayMs[key] = now;
    if (eff >= 0.995f) {
        SDL_QueueAudio(p->dev, clip.data.data(),
                       (Uint32)clip.data.size());
    } else {
        // Apply volume via SDL_MixAudioFormat into a temporary buffer.
        std::vector<Uint8> tmp(clip.data.size(), 0);
        int sdlVol = (int)(SDL_MIX_MAXVOLUME * eff);
        SDL_MixAudioFormat(tmp.data(), clip.data.data(), p->spec.format,
                           (Uint32)clip.data.size(), sdlVol);
        SDL_QueueAudio(p->dev, tmp.data(), (Uint32)tmp.size());
    }
}

void  AudioManager::setMuted(bool m)     { p->muted = m; }
bool  AudioManager::muted() const        { return p->muted; }
void  AudioManager::setVolume(float v)   { p->volume = std::clamp(v, 0.f, 1.f); }
float AudioManager::volume() const       { return p->volume; }
void  AudioManager::setMasterVolume(float v) { p->masterVolume = std::clamp(v, 0.f, 1.f); }
float AudioManager::masterVolume() const { return p->masterVolume; }
void  AudioManager::setMuteUiSfx(bool m) { p->muteUiSfx = m; }
bool  AudioManager::muteUiSfx() const    { return p->muteUiSfx; }

// ── Background music ─────────────────────────────────────────────────────────
void AudioManager::loadMusic(const std::string& path) {
    if (!p->ready || !p->musicDev) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        printf("[audio] music missing (BGM off): %s\n", path.c_str());
        return;
    }
    SDL_AudioSpec wavSpec{}; Uint8* wavBuf = nullptr; Uint32 wavLen = 0;
    if (!SDL_LoadWAV(path.c_str(), &wavSpec, &wavBuf, &wavLen)) {
        // A common mistake is renaming an .mp3 to .wav — SDL only reads real
        // RIFF/WAVE PCM, so flag that clearly.
        unsigned char hdr[4] = {0};
        { std::ifstream hf(path, std::ios::binary);
          if (hf) hf.read(reinterpret_cast<char*>(hdr), 4); }
        bool looksMp3 = (hdr[0]=='I'&&hdr[1]=='D'&&hdr[2]=='3') ||
                        (hdr[0]==0xFF && (hdr[1]&0xE0)==0xE0);
        printf("[audio] music load failed for %s: %s%s\n", path.c_str(),
               SDL_GetError(),
               looksMp3 ? "  (this file is MP3 data, not WAV — re-encode it to "
                          "a real PCM .wav)" : "");
        return;
    }
    SDL_AudioCVT cvt{};
    int rc = SDL_BuildAudioCVT(&cvt, wavSpec.format, wavSpec.channels,
                               wavSpec.freq, p->musicSpec.format,
                               p->musicSpec.channels, p->musicSpec.freq);
    std::vector<Uint8> out;
    if (rc < 0) { SDL_FreeWAV(wavBuf); return; }
    if (rc == 0) {
        out.assign(wavBuf, wavBuf + wavLen);
    } else {
        cvt.len = (int)wavLen;
        std::vector<Uint8> buf((size_t)cvt.len * cvt.len_mult, 0);
        cvt.buf = buf.data();
        std::copy(wavBuf, wavBuf + wavLen, buf.begin());
        if (SDL_ConvertAudio(&cvt) < 0) { SDL_FreeWAV(wavBuf); return; }
        out.assign(buf.begin(), buf.begin() + cvt.len_cvt);
    }
    SDL_FreeWAV(wavBuf);
    SDL_LockAudioDevice(p->musicDev);
    p->music.data = std::move(out);
    p->musicPos   = 0;
    SDL_UnlockAudioDevice(p->musicDev);
    printf("[audio] music loaded: %s (%zu bytes pcm)\n",
           path.c_str(), p->music.data.size());
}

void AudioManager::playMusic() {
    if (!p->ready || !p->musicDev || p->music.data.empty()) return;
    if (p->musicOn) return;
    SDL_LockAudioDevice(p->musicDev);
    p->musicPos = 0;
    p->musicOn  = true;
    SDL_UnlockAudioDevice(p->musicDev);
    SDL_PauseAudioDevice(p->musicDev, 0);
}

void AudioManager::stopMusic() {
    if (!p->musicDev) return;
    p->musicOn = false;
    SDL_PauseAudioDevice(p->musicDev, 1);
}

bool  AudioManager::musicPlaying() const { return p && p->musicOn; }
bool  AudioManager::musicLoaded()  const { return p && !p->music.data.empty(); }
void  AudioManager::setMusicVolume(float v) {
    if (p) p->musicVolume = std::clamp(v, 0.f, 1.f);
}
float AudioManager::musicVolume() const  { return p ? p->musicVolume : 0.f; }

bool AudioManager::isLoaded(const std::string& key) const {
    return p && p->clips.find(key) != p->clips.end();
}
int AudioManager::loadedCount() const {
    return p ? (int)p->clips.size() : 0;
}
