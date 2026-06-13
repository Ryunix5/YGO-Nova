#pragma once
#include <string>
#include <unordered_map>
#include <cstdint>

// Manages SDL2/OpenGL textures for card art
class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    // Must be called after OpenGL context is created
    void init();
    void shutdown();

    // Load a card texture by passcode. Returns ImGui texture ID (void*)
    // Falls back to the back-of-card texture if art is missing.
    void* getCardTexture(uint32_t code);
    void* getBackTexture() { return m_backTex; }
    void* getUnknownTexture() { return m_unknownTex; }
    // Human-readable card-back load status ("card_back loaded: <path>
    // size=WxH" or "procedural fallback; searched: ..."). Surfaced in the
    // Assets popup and the Copy Diagnostics payload.
    const std::string& cardBackInfo() const { return m_backInfo; }

private:
    void* loadTexture(const std::string& path);
    void* generateFallbackTexture(uint8_t r, uint8_t g, uint8_t b);
    void* generateCardBackTexture();   // procedural card back when no asset exists

    std::unordered_map<uint32_t, void*> m_cardTextures;
    void* m_backTex    = nullptr;
    void* m_unknownTex = nullptr;
    std::string m_backInfo;            // card-back load diagnostics
};
