#pragma once
#include <string>

// Minimal SFX layer built on SDL2's native audio (no SDL_mixer dependency).
// Loads WAV files, converts each clip to the device spec once, and plays them
// via SDL_QueueAudio. Missing files warn once and are silently no-op'd
// thereafter — audio never blocks gameplay or causes a crash.
class AudioManager {
public:
    AudioManager();
    ~AudioManager();
    bool init();
    void shutdown();
    bool isAvailable() const;

    // Load a WAV file under a lookup key. If the file is missing or the
    // format conversion fails, the call logs once and continues silently.
    void load(const std::string& key, const std::string& path);

    // Queue a previously loaded clip. No-op if the key is unknown, the audio
    // device failed to open, or the manager is muted.
    void play(const std::string& key);

    void  setMuted(bool m);
    bool  muted() const;
    void  setVolume(float v);
    float volume() const;

    // ── Looping background music ──────────────────────────────────────────
    // A separate streaming audio device (callback) so music mixes UNDER the
    // one-shot SFX instead of queuing behind them. loadMusic decodes a WAV;
    // playMusic loops it; stopMusic silences it. Volume is independent of SFX.
    void  loadMusic(const std::string& path);
    void  playMusic();
    void  stopMusic();
    bool  musicPlaying() const;
    bool  musicLoaded() const;
    void  setMusicVolume(float v);
    float musicVolume() const;

    // True if a clip is currently cached under `key`.
    bool isLoaded(const std::string& key) const;
    // Number of clips currently cached.
    int  loadedCount() const;
    // The canonical expected SFX bank — the names tools/generate_sfx.py
    // produces and that Game.cpp loads at startup. UI diagnostics iterate
    // this so the loaded/missing count is consistent across the app.
    static const char* const* expectedSfx();   // null-terminated
    static int               expectedSfxCount();

private:
    struct Impl;
    Impl* p;
    // SDL audio-thread callback for the streaming music device. Static so it
    // matches SDL's C callback signature; userdata is the Impl*.
    static void musicCallback(void* userdata, unsigned char* stream, int len);
};

// One process-wide audio manager, accessed by UI hooks without threading it
// through every constructor.
AudioManager& gAudio();
