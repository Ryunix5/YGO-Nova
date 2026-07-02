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

// The app runs a CORE-profile 3.3 context (Game.cpp), where the legacy
// GL_GENERATE_MIPMAP texparameter was REMOVED — setting it is ignored, so
// textures ended up with a mipmap MIN filter but no mip levels: incomplete
// sampling and the "blurry / low quality" card art testers reported. Core
// mipmaps must be built with glGenerateMipmap (GL 3.0+), which the 1.1
// Windows headers don't declare — load it once via wglGetProcAddress.
typedef void (APIENTRY* PFNGLGENERATEMIPMAPPROC)(GLenum target);
static PFNGLGENERATEMIPMAPPROC glGenerateMipmapPtr() {
#if defined(_WIN32)
    static PFNGLGENERATEMIPMAPPROC fn = []() -> PFNGLGENERATEMIPMAPPROC {
        void* p = (void*)wglGetProcAddress("glGenerateMipmap");
        // wglGetProcAddress returns small sentinel values on failure.
        if (p == nullptr || p == (void*)1 || p == (void*)2 ||
            p == (void*)3 || p == (void*)-1)
            return nullptr;
        return (PFNGLGENERATEMIPMAPPROC)p;
    }();
    return fn;
#else
    return nullptr;
#endif
}
#include <cstring>
#include <cmath>
#include <cstdio>
#include <vector>
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
    auto it = m_cardTextures.find(code);
    if (it != m_cardTextures.end()) return it->second;

    const std::string jpg = "assets/cards/" + std::to_string(code) + ".jpg";

    // While a download for this code is in flight, show the placeholder
    // without re-probing the disk every frame.
    if (m_downloadImages &&
        m_fetcher.state(code) == edo::ImageFetcher::State::InFlight)
        return m_unknownTex;

    // Try local files (bundled art, or a previously cached download).
    void* tex = loadTexture(jpg);
    if (!tex)
        tex = loadTexture("assets/cards/" + std::to_string(code) + ".png");
    if (tex) { m_cardTextures[code] = tex; return tex; }

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

void* Renderer::loadTexture(const std::string& path) {
    int w, h, ch;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!data) return nullptr;

    GLuint texID;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    // Build real mip levels (core-profile way) and filter trilinearly +
    // anisotropically so card art stays crisp at every draw size. Without
    // glGenerateMipmap we must NOT use a mipmap MIN filter — the texture
    // would be incomplete — so fall back to plain linear.
    if (auto genMips = glGenerateMipmapPtr()) {
        genMips(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        GL_LINEAR_MIPMAP_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 4.0f);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
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
