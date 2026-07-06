#include "Renderer.h"
#include <SDL.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include <GL/gl.h>
// GL_CLAMP_TO_EDGE is OpenGL 1.2+ but Windows gl.h only ships 1.1
#ifndef GL_CLAMP_TO_EDGE
#  define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_LINEAR_MIPMAP_LINEAR
#  define GL_LINEAR_MIPMAP_LINEAR 0x2703
#endif
// EXT_texture_filter_anisotropic — universally supported on desktop GL;
// sharpens minified card art beyond what trilinear alone gives.
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#  define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

// (Mip levels are built on the CPU — see uploadGammaCorrectMips below — so
// the old glGenerateMipmap loader is gone: uploading explicit levels with
// glTexImage2D works on any GL version, core profile included.)
#ifndef GL_TEXTURE_LOD_BIAS
#  define GL_TEXTURE_LOD_BIAS 0x8501
#endif
#include <cstring>
#include <cmath>
#include <cstdio>
#include <vector>

// ── Gamma-correct mip chain ─────────────────────────────────────────────────
// glGenerateMipmap box-averages RAW sRGB bytes; averaging in that non-linear
// space systematically darkens and crunches high-frequency detail — exactly
// the "pixelated card text" testers see on deck tiles. Building the chain
// ourselves with a 2x2 box in LINEAR light keeps small text legible.
static inline float srgbToLin(uint8_t v) {
    static float lut[256];
    static bool  init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) {
            float f = i / 255.f;
            lut[i] = (f <= 0.04045f) ? f / 12.92f
                                     : std::pow((f + 0.055f) / 1.055f, 2.4f);
        }
        init = true;
    }
    return lut[v];
}
static inline uint8_t linToSrgb(float f) {
    // LUT over quantised linear [0,1] → sRGB byte. Removes the per-pixel
    // pow() from the mip inner loop (the dominant cost of the gamma-correct
    // chain); 4096 steps is visually identical to the exact curve. Built on
    // first use; loadTexture only runs on the GL/main thread, so no race.
    static uint8_t lut[4097];
    static bool init = false;
    if (!init) {
        for (int i = 0; i <= 4096; ++i) {
            float x = (float)i / 4096.f;
            float s = (x <= 0.0031308f) ? x * 12.92f
                                        : 1.055f * std::pow(x, 1.f / 2.4f)
                                          - 0.055f;
            int v = (int)(s * 255.f + 0.5f);
            lut[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        init = true;
    }
    if (f <= 0.f) return 0;
    if (f >= 1.f) return 255;
    return lut[(int)(f * 4096.f + 0.5f)];
}
// Upload mip levels 1..N for the currently bound texture, halving each
// step with a linear-light 2x2 box filter. Level 0 must already be set.
static void uploadGammaCorrectMips(const unsigned char* level0, int w, int h) {
    std::vector<uint8_t> prev(level0, level0 + (size_t)w * h * 4);
    int pw = w, ph = h, level = 1;
    while (pw > 1 || ph > 1) {
        int nw = pw > 1 ? pw / 2 : 1;
        int nh = ph > 1 ? ph / 2 : 1;
        std::vector<uint8_t> next((size_t)nw * nh * 4);
        for (int y = 0; y < nh; ++y) {
            int y0 = 2 * y, y1 = (2 * y + 1 < ph) ? 2 * y + 1 : ph - 1;
            for (int x = 0; x < nw; ++x) {
                int x0 = 2 * x, x1 = (2 * x + 1 < pw) ? 2 * x + 1 : pw - 1;
                const uint8_t* p00 = &prev[((size_t)y0 * pw + x0) * 4];
                const uint8_t* p01 = &prev[((size_t)y0 * pw + x1) * 4];
                const uint8_t* p10 = &prev[((size_t)y1 * pw + x0) * 4];
                const uint8_t* p11 = &prev[((size_t)y1 * pw + x1) * 4];
                uint8_t* dst = &next[((size_t)y * nw + x) * 4];
                for (int c = 0; c < 3; ++c)
                    dst[c] = linToSrgb((srgbToLin(p00[c]) + srgbToLin(p01[c]) +
                                        srgbToLin(p10[c]) + srgbToLin(p11[c]))
                                       * 0.25f);
                dst[3] = (uint8_t)(((int)p00[3] + p01[3] + p10[3] + p11[3] + 2)
                                   / 4);
            }
        }
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, nw, nh, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, next.data());
        prev.swap(next);
        pw = nw; ph = nh; ++level;
    }
}
#include <filesystem>

// stb_image — single header image loader
#define STB_IMAGE_IMPLEMENTATION
#include "../vendor/stb/stb_image.h"

Renderer::~Renderer() { shutdown(); }

void Renderer::init() {
    // Card back: prefer a real image asset; otherwise draw a procedural
    // back. The exe usually runs from build/windows/Release while the
    // user drops the file at <project>/assets/card_back.png, so we probe
    // the SAME parent-relative bases CardDB::openAuto uses — working dir,
    // exe-relative copied assets, and up to five parents toward the
    // project root. First existing file that stbi can decode wins.
    namespace fs = std::filesystem;
    std::vector<std::string> baseDirs;
    // (0) The EXECUTABLE's own directory — the build copies assets next
    //     to the exe and users drop card_back.png there too. The working
    //     directory is often the project root (launched via build.bat),
    //     so this must be probed explicitly; SDL resolves it portably.
    if (char* sdlBase = SDL_GetBasePath()) {
        baseDirs.push_back(sdlBase);
        SDL_free(sdlBase);
    }
    // (1) Working directory + parents (launching from any build subdir).
    baseDirs.push_back(".");
    baseDirs.push_back("..");
    baseDirs.push_back("../..");
    baseDirs.push_back("../../..");
    baseDirs.push_back("../../../..");
    baseDirs.push_back("../../../../..");
    // (2) Build output as a DESCENDANT of the working directory — covers
    //     cwd = project root with the asset sitting next to the exe.
    baseDirs.push_back("build/windows/Release");
    baseDirs.push_back("build/windows/Debug");
    static const char* kNames[] = {
        "assets/card_back.png",          "assets/card_back.jpg",
        "assets/textures/card_back.png", "assets/textures/card_back.jpg",
        nullptr
    };
    m_backTex = nullptr;
    m_backInfo.clear();
    std::string searched;
    int searchedCount = 0;
    for (size_t b = 0; b < baseDirs.size() && !m_backTex; ++b) {
        for (int n = 0; kNames[n] && !m_backTex; ++n) {
            fs::path p = fs::path(baseDirs[b]) / kNames[n];
            std::error_code ec;
            if (!fs::is_regular_file(p, ec)) {
                // Keep the failure log readable: list the .png probes for
                // the first few bases only.
                if (n == 0 && searchedCount < 6) {
                    if (!searched.empty()) searched += ", ";
                    std::error_code ec2;
                    auto ap = fs::absolute(p, ec2);
                    searched += ec2 ? p.string()
                                    : ap.lexically_normal().string();
                    ++searchedCount;
                }
                continue;
            }
            std::string abs = fs::absolute(p, ec).lexically_normal().string();
            if (ec) abs = p.string();
            int iw = 0, ih = 0, ich = 0;
            if (!stbi_info(p.string().c_str(), &iw, &ih, &ich)) {
                printf("[Renderer] card_back found but unreadable: %s (%s)\n",
                       abs.c_str(), stbi_failure_reason());
                if (!searched.empty()) searched += ", ";
                searched += abs + " (unreadable: " +
                            std::string(stbi_failure_reason()) + ")";
                continue;
            }
            m_backTex = loadTexture(p.string());
            if (m_backTex) {
                m_backInfo = "card_back loaded: " + abs + "  size=" +
                             std::to_string(iw) + "x" + std::to_string(ih);
                printf("[Renderer] %s\n", m_backInfo.c_str());
            } else {
                printf("[Renderer] card_back decode failed: %s (%s)\n",
                       abs.c_str(), stbi_failure_reason());
                if (!searched.empty()) searched += ", ";
                searched += abs + " (decode failed)";
            }
        }
    }
    if (!m_backTex) {
        m_backTex = generateCardBackTexture();
        m_backInfo = "card_back NOT found — procedural fallback in use; "
                     "searched: " + searched;
        printf("[Renderer] card_back NOT found; searched: %s\n",
               searched.c_str());
        printf("[Renderer] procedural card back enabled\n");
    }
    m_unknownTex = generateFallbackTexture(60, 40, 80);   // purple — unknown card

    // Background card-art downloader (online releases ship without the ~9k
    // images; they are fetched + cached on first view).
    m_fetcher.setEnabled(m_downloadImages);
    m_fetcher.start();
}

void* Renderer::loadCachedImage(const std::string& path) {
    auto it = m_imageCache.find(path);
    if (it != m_imageCache.end()) return it->second;
    void* t = loadTexture(path);
    m_imageCache[path] = t;          // cache even nullptr so it won't retry
    return t;
}

bool Renderer::setCardBack(const std::string& path) {
    void* tex = loadTexture(path);
    if (!tex) return false;
    // Free the previous back (procedural fallback or an earlier sleeve) so
    // switching sleeves repeatedly doesn't leak GL textures.
    if (m_backTex) {
        GLuint old = (GLuint)(uintptr_t)m_backTex;
        glDeleteTextures(1, &old);
    }
    m_backTex  = tex;
    m_backInfo = "card_back (sleeve): " + path;
    return true;
}

void Renderer::shutdown() {
    m_fetcher.stop();
    for (auto& [code, tex] : m_cardTextures) {
        GLuint id = (GLuint)(uintptr_t)tex;
        glDeleteTextures(1, &id);
    }
    m_cardTextures.clear();

    if (m_backTex)    { GLuint id=(GLuint)(uintptr_t)m_backTex;    glDeleteTextures(1,&id); m_backTex=nullptr;    }
    if (m_unknownTex) { GLuint id=(GLuint)(uintptr_t)m_unknownTex; glDeleteTextures(1,&id); m_unknownTex=nullptr; }
}

void* Renderer::getCardTexture(uint32_t code) {
    const std::string jpg = "assets/cards/" + std::to_string(code) + ".jpg";

    auto it = m_cardTextures.find(code);
    if (it != m_cardTextures.end()) {
        // A low-res upgrade finished downloading: drop the stale texture and
        // fall through to reload the full-size file.
        if (!m_upgradingArt.empty() && m_upgradingArt.count(code) &&
            m_fetcher.state(code) == edo::ImageFetcher::State::Done) {
            GLuint id = (GLuint)(uintptr_t)it->second;
            if (it->second && it->second != m_unknownTex)
                glDeleteTextures(1, &id);
            m_cardTextures.erase(it);
            m_upgradingArt.erase(code);
        } else {
            return it->second;
        }
    }

    // While a download for this code is in flight, show the placeholder
    // (first fetch) or keep using the old file (upgrade) without re-probing
    // the disk every frame.
    if (m_downloadImages && !m_upgradingArt.count(code) &&
        m_fetcher.state(code) == edo::ImageFetcher::State::InFlight)
        return m_unknownTex;

    // Per-frame decode budget: a fresh decode + gamma mip build is a few ms,
    // so cap how many NEW cards we upload per frame. When it's spent, return
    // the placeholder WITHOUT caching — the next frame retries — so a whole
    // deck/board spreads its loads over a handful of frames instead of one
    // long hitch. Already-cached cards above never reach here, so scrolling a
    // loaded collection stays free.
    if (m_texBudgetLeft <= 0)
        return m_unknownTex;

    // Try local files (bundled art, or a previously cached download).
    int w = 0, h = 0;
    void* tex = loadTexture(jpg, &w, &h);
    if (!tex)
        tex = loadTexture("assets/cards/" + std::to_string(code) + ".png",
                          &w, &h);
    if (tex) {
        --m_texBudgetLeft;         // only a real decode spends the budget
        // Old bundled packs shipped 177x254 thumbnails — too small for the
        // preview panel and hand. Kick a full-size re-download (the CDN serves
        // 813x1185); the texture swaps in the moment it lands.
        if (m_downloadImages && w > 0 && w < 350 &&
            m_fetcher.state(code) == edo::ImageFetcher::State::None) {
            m_fetcher.request(code, jpg);
            m_upgradingArt.insert(code);
        }
        m_cardTextures[code] = tex;
        return tex;
    }

    // Not on disk. Kick off an on-demand download and show the placeholder.
    // CRUCIAL: do NOT cache the placeholder while a fetch could still land —
    // the next frame re-checks disk once the download completes.
    if (m_downloadImages) {
        switch (m_fetcher.state(code)) {
            case edo::ImageFetcher::State::None:
                m_fetcher.request(code, jpg);
                return m_unknownTex;                  // uncached — retry later
            case edo::ImageFetcher::State::InFlight:
                return m_unknownTex;                  // uncached — retry later
            default:                                  // Done(missing) / Failed
                break;                                // give up below
        }
    }

    m_cardTextures[code] = m_unknownTex;   // cache the fallback — stop retrying
    return m_unknownTex;
}

void Renderer::prefetchCard(uint32_t code) {
    if (!m_downloadImages || code == 0) return;
    if (m_cardTextures.count(code)) return;          // already uploaded
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string base = "assets/cards/" + std::to_string(code);
    if (fs::is_regular_file(base + ".jpg", ec) ||
        fs::is_regular_file(base + ".png", ec))
        return;                                       // already on disk
    if (m_fetcher.state(code) == edo::ImageFetcher::State::None)
        m_fetcher.request(code, base + ".jpg");
}

void* Renderer::loadTexture(const std::string& path, int* outW, int* outH) {
    int w, h, ch;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!data) return nullptr;
    if (outW) *outW = w;
    if (outH) *outH = h;

    GLuint texID;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    // Hand-built gamma-correct mip chain + trilinear + anisotropic, with a
    // small negative LOD bias so sampling favours the sharper mip level —
    // together these keep the tiny card text readable instead of the
    // crunchy "pixelated" look glGenerateMipmap's sRGB box filter gives.
    uploadGammaCorrectMips(data, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.0f);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, -0.35f);
    stbi_image_free(data);

    return (void*)(uintptr_t)texID;
}

// Procedural card back in the style of the official Yu-Gi-Oh back: warm
// brown field, tan double border, and a dark central swirl-vortex oval.
// This is a hand-drawn approximation — for the genuine artwork drop the
// real image at assets/card_back.png and the loader will prefer it
// automatically. Rounded corners are cut via the alpha channel so the
// back looks right at small zone sizes and in the large preview.
void* Renderer::generateCardBackTexture() {
    const int W = 256, H = 373;             // ~421:614 card aspect, 2x res
    static std::vector<uint8_t> px;
    px.assign((size_t)W * H * 4, 0);
    const float cx = W * 0.5f, cy = H * 0.5f;
    const float cornerR = 18.f;             // rounded-corner radius
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            size_t i = ((size_t)y * W + x) * 4;
            // Rounded-corner alpha cut.
            float qx = (x < W / 2) ? (float)x : (float)(W - 1 - x);
            float qy = (y < H / 2) ? (float)y : (float)(H - 1 - y);
            uint8_t a = 255;
            if (qx < cornerR && qy < cornerR) {
                float dcx = cornerR - qx, dcy = cornerR - qy;
                float dc  = std::sqrt(dcx * dcx + dcy * dcy);
                if (dc > cornerR)        a = 0;
                else if (dc > cornerR-2) a = (uint8_t)(255*(cornerR-dc)*0.5f);
            }
            float edge = qx < qy ? qx : qy;          // distance to border
            float dx = x - cx, dy = y - cy;
            float d  = std::sqrt(dx * dx + dy * dy);

            float r, g, b;
            if (edge < 6.f) {                        // dark outer frame
                r = 38; g = 26; b = 16;
            } else if (edge < 9.f) {                 // tan border line
                r = 196; g = 162; b = 110;
            } else if (edge < 12.f) {                // thin dark separator
                r = 70; g = 48; b = 28;
            } else {
                // Warm brown field with a gentle radial vignette — the
                // classic card-back base tone.
                float vig = d / (W * 1.05f);
                if (vig > 1.f) vig = 1.f;
                r = 122.f - 26.f * vig;
                g =  82.f - 18.f * vig;
                b =  46.f - 12.f * vig;
                // Central swirl-vortex oval (taller than wide).
                float exn = dx;
                float eyn = dy / 1.42f;
                float ed  = std::sqrt(exn * exn + eyn * eyn);
                const float OR = 84.f;               // oval radius
                if (std::fabs(ed - OR) < 2.6f) {     // tan oval outline
                    r = 206; g = 172; b = 116;
                } else if (std::fabs(ed - OR) < 5.2f) {
                    r = 54; g = 36; b = 22;          // dark rim band
                } else if (ed < OR) {
                    // Swirl: brightness follows a spiral phase so the
                    // vortex reads as curved arms winding to the centre.
                    float ang = std::atan2(eyn, exn);
                    float sw  = 0.5f + 0.5f * std::sin(ang * 2.f + ed * 0.16f);
                    float fade= 1.f - ed / (OR + 8.f);
                    float k   = sw * fade;
                    r = 26.f + 116.f * k;
                    g = 18.f +  82.f * k;
                    b = 12.f +  46.f * k;
                    // Bright core at the vortex eye.
                    if (ed < 10.f) {
                        float c2 = 1.f - ed / 10.f;
                        r += 80.f * c2; g += 62.f * c2; b += 40.f * c2;
                    }
                }
            }
            if (r > 255.f) r = 255.f; if (g > 255.f) g = 255.f;
            if (b > 255.f) b = 255.f;
            px[i] = (uint8_t)r; px[i+1] = (uint8_t)g; px[i+2] = (uint8_t)b;
            px[i+3] = a;
        }
    }
    GLuint texID;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 px.data());
    px.clear(); px.shrink_to_fit();
    return (void*)(uintptr_t)texID;
}

void* Renderer::generateFallbackTexture(uint8_t r, uint8_t g, uint8_t b) {
    // 4x4 solid-colour texture
    const int W = 4, H = 4;
    uint8_t pixels[W * H * 4];
    for (int i = 0; i < W * H; i++) {
        pixels[i*4+0] = r;
        pixels[i*4+1] = g;
        pixels[i*4+2] = b;
        pixels[i*4+3] = 255;
    }
    GLuint texID;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return (void*)(uintptr_t)texID;
}
