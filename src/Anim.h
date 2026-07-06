#pragma once
// ─── Anim.h ────────────────────────────────────────────────────────────────
//
// Header-only animation layer for the duel screen. The renderer is purely
// presentational: animations are timed soft pulses / rings / beams / LP
// flashes / floating damage numbers, and they NEVER touch engine state.
// They live until their `dur` elapses and then evict themselves on render.
//
// Triggered from UI.cpp at exactly the points where SFX are already queued
// (state-delta observer in drawDuel + click handlers in the action popup),
// and rendered in drawField after the field paint so they sit on top.
//
// Design constraints:
//   * non-blocking — no animation gates the engine, all rendering is fire-
//     and-forget.
//   * bounded — clear() drops everything on duel reset / return to lobby.
//   * ImGui-only — uses ImDrawList; no extra shader/texture work needed.
//
#include "imgui.h"
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cfloat>

namespace edo {

// ── Animation presentation config ────────────────────────────────────────────
// Mirrors the user-facing settings (Settings.h anim* keys). The UI pushes
// this into AnimManager::setConfig() whenever a setting changes. Emitters
// consult it so disabling a category is a no-op at the source — nothing
// queued, nothing rendered, zero cost.
struct AnimConfig {
    bool  enabled       = true;   // master switch — off = no animations at all
    bool  bigSummons    = true;   // boss/large-monster centre entrance
    bool  phaseBanners  = true;   // centre phase banner on phase change
    bool  screenShake   = true;   // brief board jitter on major events
    bool  reduceMotion  = false;  // accessibility: short fades, no shake/boss
    float speed         = 1.0f;   // 0.5 / 1 / 2; <=0 treated as "instant"
    // Phase presentation delay in seconds — purely cosmetic hold so phase
    // changes feel intentional. NEVER gates the engine or networking.
    float phaseDelay    = 0.45f;
};

struct Anim {
    enum Type {
        Pulse,        // soft filled disc that grows + fades
        Ring,         // expanding ring outline
        Beam,         // line from a → b that fades
        LPFlash,      // rectangular flash on a player's LP panel
        DamageNum,    // floating "-NNN" number above origin
        ZoneFlash,    // rectangular flash on a zone (placement / send to GY)
        // ── Stage A additions ─────────────────────────────────────────────
        Banner,       // centre-screen phase banner (slide-in / hold / fade)
        BossCard,     // enlarged card entrance with energy ring + type label
        CardTrail,    // a card-sized ghost gliding a → b (draw / send / banish)
        // ── Chain / targeting feedback ─────────────────────────────────────
        ChainPop,     // burst ring + card name + "CHAIN n" badge on activation
        TargetLock,   // converging crosshair reticle on a targeted card
        // ── Comprehension pack ─────────────────────────────────────────────
        SummonBurst,  // colored ring + type label at a zone (Synchro/Xyz/…)
        Reveal,       // a card shown large in centre, then shrinks to a zone
        Vortex,       // purple dimensional swirl (banish)
        Negate        // shatter shards + red ✕ + "NEGATED" over a card
    };
    Type     type   = Pulse;
    double   start  = 0.0;       // ImGui::GetTime() seconds when queued
    double   dur    = 0.5;
    ImVec2   a {};               // primary point / rect top-left
    ImVec2   b {};               // beam end / rect bottom-right
    ImU32    color  = IM_COL32_WHITE;
    float    radius = 24.f;
    int      ivalue = 0;
    char     text[40] = {};      // banner / damage label (widened for banners)
    char     text2[40] = {};     // secondary line — ChainPop effect verb, etc.
    void*    tex    = nullptr;   // card texture for BossCard / CardTrail / Reveal
};

class AnimManager {
public:
    void clear()                  { m_items.clear(); m_shakeStart = -1.0; }
    bool empty()         const    { return m_items.empty(); }
    size_t count()       const    { return m_items.size(); }

    // ── Config ────────────────────────────────────────────────────────────
    void setConfig(const AnimConfig& c) { m_cfg = c; }
    const AnimConfig& config() const    { return m_cfg; }
    // Scales a base duration by the user's speed setting. "Instant" (speed
    // <= 0) collapses everything to a single frame so nothing lingers.
    double sdur(double base) const {
        if (!m_cfg.enabled) return 0.0;
        float sp = m_cfg.speed;
        if (sp <= 0.f) return 0.001;             // instant
        double d = base / (double)sp;
        if (m_cfg.reduceMotion) d *= 0.55;       // shorter fades
        return d;
    }

    // ── Convenience emitters ──────────────────────────────────────────────
    // Field-assigned construction avoids relying on the exact aggregate-
    // initialization rules across C++ standards (default member initializers
    // + aggregate init has been quirky pre-C++20).
    void pulse(ImVec2 center, float r, ImU32 col, double dur = 0.45) {
        Anim it; it.type = Anim::Pulse; it.start = ImGui::GetTime();
        it.dur = dur; it.a = center; it.color = col; it.radius = r;
        push(it);
    }
    void ring(ImVec2 center, float r, ImU32 col, double dur = 0.55) {
        Anim it; it.type = Anim::Ring;  it.start = ImGui::GetTime();
        it.dur = dur; it.a = center; it.color = col; it.radius = r;
        push(it);
    }
    void beam(ImVec2 from, ImVec2 to, ImU32 col, double dur = 0.45) {
        Anim it; it.type = Anim::Beam;  it.start = ImGui::GetTime();
        it.dur = dur; it.a = from; it.b = to; it.color = col;
        push(it);
    }
    void zoneFlash(ImVec2 tl, ImVec2 br, ImU32 col, double dur = 0.55) {
        Anim it; it.type = Anim::ZoneFlash; it.start = ImGui::GetTime();
        it.dur = dur; it.a = tl; it.b = br; it.color = col;
        push(it);
    }
    void lpFlash(ImVec2 tl, ImVec2 br, ImU32 col, double dur = 0.55) {
        Anim it; it.type = Anim::LPFlash; it.start = ImGui::GetTime();
        it.dur = dur; it.a = tl; it.b = br; it.color = col;
        push(it);
    }
    void damageNum(ImVec2 origin, int dmg, ImU32 col, double dur = 1.10) {
        Anim it; it.type = Anim::DamageNum; it.start = ImGui::GetTime();
        it.dur = dur; it.a = origin; it.color = col; it.ivalue = dmg;
        snprintf(it.text, sizeof(it.text), "-%d", dmg);
        push(it);
    }
    // Same visual treatment as damageNum but with a custom prefixed label —
    // used for healing ("+500"), debug counts, etc.
    void floatText(ImVec2 origin, const char* text, ImU32 col,
                   double dur = 1.10) {
        Anim it; it.type = Anim::DamageNum; it.start = ImGui::GetTime();
        it.dur = dur; it.a = origin; it.color = col;
        std::snprintf(it.text, sizeof(it.text), "%s", text ? text : "");
        push(it);
    }

    // ── Stage A emitters ──────────────────────────────────────────────────
    // Centre-screen phase banner. `screenSize` is the duel window size so
    // the banner can centre itself. No-op when phase banners are disabled.
    void emitPhaseBanner(const char* phaseName, ImVec2 screenSize,
                         ImU32 col) {
        if (!m_cfg.enabled || !m_cfg.phaseBanners) return;
        if (!phaseName || !phaseName[0]) return;
        Anim it; it.type = Anim::Banner; it.start = ImGui::GetTime();
        // Total banner life = fade-in/out envelope (~0.55s) + the user's
        // cosmetic phase-delay hold. This is the "small delay between
        // phases" — purely a presentation hold; it NEVER blocks the engine
        // or the network (the snapshot/phase has already advanced).
        it.dur = 0.55 + (double)m_cfg.phaseDelay;
        it.a = screenSize; it.color = col;
        std::snprintf(it.text, sizeof(it.text), "%s", phaseName);
        push(it);
    }
    // Big-monster / boss entrance. `tex` is the card texture (may be null —
    // falls back to a glowing plate). `zoneRect` lets the card settle toward
    // its destination near the end. No-op if big summons / motion are off.
    void emitBossSummon(void* tex, ImVec2 screenSize, ImVec2 zoneCenter,
                        const char* typeLabel, ImU32 col) {
        if (!m_cfg.enabled || !m_cfg.bigSummons || m_cfg.reduceMotion) return;
        Anim it; it.type = Anim::BossCard; it.start = ImGui::GetTime();
        it.dur = 1.30; it.a = screenSize; it.b = zoneCenter;
        it.color = col; it.tex = tex;
        std::snprintf(it.text, sizeof(it.text), "%s",
                      typeLabel ? typeLabel : "");
        push(it);
    }
    // Chain activation burst: a gold ring snaps out from the activating card
    // with its name + a "CHAIN n" link badge floating above. Makes an effect
    // activation (especially the AI's) clearly visible. `link` 0 hides the
    // badge. Rendered on the foreground list by renderTop so it sits on top.
    void chainPop(ImVec2 center, const char* name, int link, ImU32 col,
                  const char* verb = nullptr, double dur = 1.25) {
        Anim it; it.type = Anim::ChainPop; it.start = ImGui::GetTime();
        it.dur = dur; it.a = center; it.color = col; it.ivalue = link;
        std::snprintf(it.text, sizeof(it.text), "%s", name ? name : "");
        std::snprintf(it.text2, sizeof(it.text2), "%s", verb ? verb : "");
        push(it);
    }
    // Colored summon-type entrance at a zone — an expanding ring + a type word
    // (SYNCHRO / XYZ / FUSION / LINK / RITUAL / SUMMON). `r` ~ half the tile.
    void summonBurst(ImVec2 center, float r, const char* typeLabel, ImU32 col,
                     double dur = 0.85) {
        Anim it; it.type = Anim::SummonBurst; it.start = ImGui::GetTime();
        it.dur = dur; it.a = center; it.radius = r; it.color = col;
        std::snprintf(it.text, sizeof(it.text), "%s", typeLabel ? typeLabel : "");
        push(it);
    }
    // Reveal a searched/added card: shown large at `from`, then shrinks toward
    // `to` (the hand). `tex` is the card art; `name` labels it.
    void reveal(ImVec2 from, ImVec2 to, void* tex, const char* name,
                double dur = 1.15) {
        Anim it; it.type = Anim::Reveal; it.start = ImGui::GetTime();
        it.dur = dur; it.a = from; it.b = to; it.tex = tex;
        it.color = IM_COL32(120, 200, 255, 255);
        std::snprintf(it.text, sizeof(it.text), "%s", name ? name : "");
        push(it);
    }
    // Banish vortex — a purple dimensional swirl centred on a card.
    void vortex(ImVec2 center, double dur = 0.75) {
        Anim it; it.type = Anim::Vortex; it.start = ImGui::GetTime();
        it.dur = dur; it.a = center; it.color = IM_COL32(186, 116, 240, 255);
        push(it);
    }
    // Negate — shatter shards + a red ✕ + "NEGATED" over the negated card.
    void negate(ImVec2 center, const char* name, double dur = 1.05) {
        Anim it; it.type = Anim::Negate; it.start = ImGui::GetTime();
        it.dur = dur; it.a = center; it.color = IM_COL32(255, 70, 70, 255);
        std::snprintf(it.text, sizeof(it.text), "%s", name ? name : "");
        push(it);
    }
    // Targeting reticle: four corner brackets converge inward onto a card with
    // a crosshair + spin, signalling "this card got targeted".
    void targetLock(ImVec2 center, ImU32 col, double dur = 0.85) {
        Anim it; it.type = Anim::TargetLock; it.start = ImGui::GetTime();
        it.dur = dur; it.a = center; it.color = col;
        push(it);
    }
    // Card-sized ghost gliding from a → b (draw, send-to-GY, banish trail).
    void cardTrail(ImVec2 from, ImVec2 to, void* tex, ImU32 col,
                   double dur = 0.5) {
        Anim it; it.type = Anim::CardTrail; it.start = ImGui::GetTime();
        it.dur = dur; it.a = from; it.b = to; it.color = col; it.tex = tex;
        push(it);
    }
    // Trigger a brief decaying board jitter. Honoured by shakeOffset(),
    // which drawDuel adds to the field child's origin so cards + hit-tests
    // move together. No-op when shake / motion settings forbid it.
    void emitShake(float magnitude = 7.f, double dur = 0.32) {
        if (!m_cfg.enabled || !m_cfg.screenShake || m_cfg.reduceMotion) return;
        m_shakeStart = ImGui::GetTime();
        m_shakeDur   = dur / (m_cfg.speed > 0.f ? m_cfg.speed : 1.f);
        m_shakeMag   = magnitude;
    }
    // Current decaying shake offset (zero when no shake active). Pseudo-
    // random but deterministic per-frame via the clock, so it reads as a
    // jitter rather than a smooth slide.
    ImVec2 shakeOffset() const {
        if (m_shakeStart < 0.0) return {0.f, 0.f};
        double t = (ImGui::GetTime() - m_shakeStart) / m_shakeDur;
        if (t >= 1.0 || t < 0.0) return {0.f, 0.f};
        float decay = 1.f - (float)t;
        float ph = (float)(ImGui::GetTime() * 53.0);
        float ox = std::sin(ph * 1.7f) * m_shakeMag * decay;
        float oy = std::cos(ph * 2.3f) * m_shakeMag * decay;
        return {ox, oy};
    }

    // ── Per-frame render ──────────────────────────────────────────────────
    void render(ImDrawList* dl) {
        if (!dl) return;
        const double now = ImGui::GetTime();
        for (size_t i = 0; i < m_items.size(); ) {
            Anim& it = m_items[i];
            double t = (it.dur > 0.0) ? (now - it.start) / it.dur : 1.0;
            if (t >= 1.0) { m_items.erase(m_items.begin() + i); continue; }
            if (t < 0.0)  t = 0.0;
            float a = 1.0f - (float)t;
            switch (it.type) {
            case Anim::Pulse: {
                float r = it.radius * (1.f + 0.55f * (float)t);
                ImU32 fill = withAlpha(it.color, (unsigned)(70  * a));
                ImU32 ring = withAlpha(it.color, (unsigned)(220 * a));
                dl->AddCircleFilled(it.a, r, fill, 32);
                dl->AddCircle      (it.a, r, ring, 32, 2.0f);
                break;
            }
            case Anim::Ring: {
                float r = it.radius * (0.45f + 1.15f * (float)t);
                ImU32 col = withAlpha(it.color, (unsigned)(240 * a));
                dl->AddCircle(it.a, r, col, 36, 3.0f * a + 0.5f);
                break;
            }
            case Anim::Beam: {
                ImU32 col = withAlpha(it.color, (unsigned)(255 * a));
                float thick = 4.5f * a + 1.2f;
                // Faint wide underlay so the shaft reads on a busy board.
                dl->AddLine(it.a, it.b,
                            withAlpha(it.color, (unsigned)(70 * a)),
                            thick + 4.f);
                dl->AddLine(it.a, it.b, col, thick);
                // Directional arrowhead at the impact end — turns the beam
                // into a clear "X attacks Y" strike, not just a flash.
                float dx = it.b.x - it.a.x, dy = it.b.y - it.a.y;
                float len = std::sqrt(dx * dx + dy * dy);
                if (len > 1.f) {
                    dx /= len; dy /= len;
                    float nx = -dy, ny = dx;          // perpendicular
                    float hl = 20.f * a + 7.f;        // head length
                    float hw = 10.f * a + 3.5f;       // head half-width
                    ImVec2 base{ it.b.x - dx * hl, it.b.y - dy * hl };
                    ImVec2 p1{ base.x + nx * hw, base.y + ny * hw };
                    ImVec2 p2{ base.x - nx * hw, base.y - ny * hw };
                    dl->AddTriangleFilled(it.b, p1, p2, col);
                } else {
                    dl->AddCircleFilled(it.b, 6.0f * a + 2.0f,
                        withAlpha(it.color, (unsigned)(180 * a)), 16);
                }
                break;
            }
            case Anim::LPFlash:
            case Anim::ZoneFlash: {
                ImU32 fill = withAlpha(it.color, (unsigned)(160 * a));
                ImU32 brd  = withAlpha(it.color, (unsigned)(220 * a));
                dl->AddRectFilled(it.a, it.b, fill, 6.f);
                dl->AddRect      (it.a, it.b, brd,  6.f, 0, 2.f);
                break;
            }
            case Anim::DamageNum: {
                float dy = -44.f * (float)t;
                ImU32 col = withAlpha(it.color, (unsigned)(255 * a));
                ImU32 sh  = withAlpha(IM_COL32(0,0,0,255), (unsigned)(170 * a));
                ImVec2 ts = ImGui::CalcTextSize(it.text);
                ImVec2 p { it.a.x - ts.x * 0.5f, it.a.y + dy };
                dl->AddText({p.x + 2.f, p.y + 2.f}, sh,  it.text);
                dl->AddText( p,                    col, it.text);
                break;
            }
            case Anim::CardTrail: {
                // Ghost card gliding a → b with an ease-out path + fading
                // trail dots behind it. Card aspect 421:614.
                float e  = 1.f - (1.f - (float)t) * (1.f - (float)t);  // ease-out
                ImVec2 c { it.a.x + (it.b.x - it.a.x) * e,
                           it.a.y + (it.b.y - it.a.y) * e };
                float cw = 26.f, ch = cw * (614.f / 421.f);
                ImVec2 p0 { c.x - cw * 0.5f, c.y - ch * 0.5f };
                ImVec2 p1 { c.x + cw * 0.5f, c.y + ch * 0.5f };
                // Trailing dots.
                for (int k = 1; k <= 3; ++k) {
                    float tk = e - k * 0.06f; if (tk < 0.f) continue;
                    ImVec2 tc { it.a.x + (it.b.x - it.a.x) * tk,
                                it.a.y + (it.b.y - it.a.y) * tk };
                    dl->AddCircleFilled(tc, 4.f - k,
                        withAlpha(it.color, (unsigned)(120 * a / k)), 12);
                }
                if (it.tex) {
                    dl->AddImageRounded((ImTextureID)it.tex, p0, p1,
                        {0,0}, {1,1},
                        withAlpha(IM_COL32_WHITE, (unsigned)(235 * a)), 3.f);
                } else {
                    dl->AddRectFilled(p0, p1,
                        withAlpha(it.color, (unsigned)(200 * a)), 3.f);
                }
                dl->AddRect(p0, p1,
                    withAlpha(it.color, (unsigned)(235 * a)), 3.f, 0, 1.5f);
                break;
            }
            case Anim::TargetLock: {
                // Four corner brackets converge from wide to a tight box,
                // plus a crosshair + a slowly spinning ring. Reads as a lock-on.
                float conv = (t < 0.4f) ? (float)(t / 0.4) : 1.f;   // 0→1 settle
                float spread = 46.f - 24.f * conv;                  // box half-size
                float br = 11.f;                                    // bracket arm
                ImU32 col = withAlpha(it.color, (unsigned)(255 * a));
                ImVec2 c = it.a;
                ImVec2 cn[4] = {
                    {c.x - spread, c.y - spread}, {c.x + spread, c.y - spread},
                    {c.x + spread, c.y + spread}, {c.x - spread, c.y + spread} };
                // Corner brackets (L-shapes pointing inward).
                float th = 2.4f;
                dl->AddLine(cn[0], {cn[0].x + br, cn[0].y}, col, th);
                dl->AddLine(cn[0], {cn[0].x, cn[0].y + br}, col, th);
                dl->AddLine(cn[1], {cn[1].x - br, cn[1].y}, col, th);
                dl->AddLine(cn[1], {cn[1].x, cn[1].y + br}, col, th);
                dl->AddLine(cn[2], {cn[2].x - br, cn[2].y}, col, th);
                dl->AddLine(cn[2], {cn[2].x, cn[2].y - br}, col, th);
                dl->AddLine(cn[3], {cn[3].x + br, cn[3].y}, col, th);
                dl->AddLine(cn[3], {cn[3].x, cn[3].y - br}, col, th);
                // Spinning ring + crosshair.
                float rr = spread * 0.62f;
                dl->AddCircle(c, rr, withAlpha(it.color, (unsigned)(160 * a)),
                              24, 1.6f);
                dl->AddLine({c.x - rr - 4, c.y}, {c.x - 4, c.y}, col, 1.6f);
                dl->AddLine({c.x + 4, c.y}, {c.x + rr + 4, c.y}, col, 1.6f);
                dl->AddLine({c.x, c.y - rr - 4}, {c.x, c.y - 4}, col, 1.6f);
                dl->AddLine({c.x, c.y + 4}, {c.x, c.y + rr + 4}, col, 1.6f);
                break;
            }
            case Anim::SummonBurst: {
                // Expanding colored ring + a couple of trailing rings + the
                // summon-type word fading above the zone.
                float r = it.radius * (0.5f + 1.4f * (float)t);
                dl->AddCircle(it.a, r, withAlpha(it.color, (unsigned)(230 * a)),
                              40, 3.0f * a + 0.8f);
                dl->AddCircle(it.a, r * 0.7f,
                              withAlpha(it.color, (unsigned)(140 * a)), 36, 1.6f);
                if (it.text[0]) {
                    ImFont* font = ImGui::GetFont();
                    float fsz = 16.f;
                    ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.f, it.text);
                    ImVec2 tp { it.a.x - ts.x * 0.5f,
                                it.a.y - it.radius - 14.f - 10.f * (float)t };
                    dl->AddText(font, fsz, {tp.x + 1.5f, tp.y + 1.5f},
                        withAlpha(IM_COL32(0,0,0,255), (unsigned)(170 * a)), it.text);
                    dl->AddText(font, fsz, tp,
                        withAlpha(it.color, (unsigned)(255 * a)), it.text);
                }
                break;
            }
            case Anim::Vortex: {
                // Concentric spiralling arcs collapsing inward — a banish rift.
                float spin = (float)t * 8.0f;
                for (int k = 0; k < 3; ++k) {
                    float rr = (40.f - k * 10.f) * (1.f - 0.5f * (float)t);
                    int seg = 20;
                    for (int s = 0; s < seg; ++s) {
                        float a0 = spin + k * 1.1f + (float)s / seg * 6.2831853f;
                        float a1 = a0 + 0.20f;
                        ImVec2 p0 { it.a.x + std::cos(a0) * rr,
                                    it.a.y + std::sin(a0) * rr };
                        ImVec2 p1 { it.a.x + std::cos(a1) * rr,
                                    it.a.y + std::sin(a1) * rr };
                        dl->AddLine(p0, p1,
                            withAlpha(it.color, (unsigned)(220 * a)), 2.0f);
                    }
                }
                dl->AddCircleFilled(it.a, 6.f * (1.f - (float)t) + 2.f,
                    withAlpha(it.color, (unsigned)(180 * a)), 16);
                break;
            }
            case Anim::Banner:
            case Anim::BossCard:
            case Anim::ChainPop:
            case Anim::Reveal:
            case Anim::Negate:
                // Overlay types — drawn by renderTop() on the foreground
                // list so they sit above the info panel + zones. Skipped
                // here, but still evicted by the t>=1 check above.
                break;
            }
            ++i;
        }
    }

    // ── Overlay render — banner + boss entrance on the foreground list ────
    // Called by UI after the field paint with ImGui::GetForegroundDrawList()
    // and the duel-window top-left origin. Does NOT evict (render() owns the
    // lifetime); it only draws the still-alive Banner / BossCard items.
    void renderTop(ImDrawList* dl, ImVec2 winTL) {
        if (!dl) return;
        const double now = ImGui::GetTime();
        for (const Anim& it : m_items) {
            double t = (it.dur > 0.0) ? (now - it.start) / it.dur : 1.0;
            if (t < 0.0 || t >= 1.0) continue;
            if (it.type == Anim::Banner)   drawBanner(dl, winTL, it, t);
            else if (it.type == Anim::BossCard) drawBoss(dl, winTL, it, t);
            else if (it.type == Anim::ChainPop) drawChainPop(dl, it, t);
            else if (it.type == Anim::Reveal)   drawReveal(dl, it, t);
            else if (it.type == Anim::Negate)   drawNegate(dl, it, t);
        }
    }

    // Mostly for safety — keep memory bounded if a bug spammed pushes.
    void cap(size_t maxItems = 64) {
        if (m_items.size() > maxItems)
            m_items.erase(m_items.begin(),
                          m_items.begin() + (m_items.size() - maxItems));
    }

private:
    static ImU32 withAlpha(ImU32 col, unsigned a) {
        if (a > 255) a = 255;
        return (col & 0x00FFFFFFu) | (a << 24);
    }
    // Single choke point: applies the speed / reduce-motion scaling to EVERY
    // queued animation (existing emitters + new ones) so settings take effect
    // without touching individual call sites. Master-disable drops the push.
    void push(Anim a) {
        if (!m_cfg.enabled) return;
        float sp = m_cfg.speed;
        if (sp <= 0.f)            a.dur = 0.001;          // instant
        else {
            a.dur /= (double)sp;
            if (m_cfg.reduceMotion) a.dur *= 0.55;        // shorter fades
        }
        m_items.push_back(a); cap();
    }

    // ── Banner: centre phase plate, slide-in / hold / fade-out ────────────
    void drawBanner(ImDrawList* dl, ImVec2 winTL, const Anim& it, double t) {
        float W = it.a.x, H = it.a.y;
        // Envelope: 0-.16 slide+fade in, .16-.66 hold, .66-1 fade out.
        float alpha, slide;
        if (t < 0.16)      { float k = (float)(t / 0.16); alpha = k; slide = (1.f - k) * 60.f; }
        else if (t < 0.66) { alpha = 1.f; slide = 0.f; }
        else               { float k = (float)((t - 0.66) / 0.34); alpha = 1.f - k; slide = -k * 40.f; }
        if (alpha < 0.f) alpha = 0.f;
        float cx = winTL.x + W * 0.5f + slide;
        float cy = winTL.y + H * 0.42f;
        const float bw = 460.f, bh = 64.f;
        ImVec2 a { cx - bw * 0.5f, cy - bh * 0.5f };
        ImVec2 b { cx + bw * 0.5f, cy + bh * 0.5f };
        // Wide soft glow band behind the plate.
        dl->AddRectFilled({winTL.x, a.y - 6.f}, {winTL.x + W, b.y + 6.f},
            withAlpha(it.color, (unsigned)(26 * alpha)), 0.f);
        // Plate: dark glass with a coloured gradient core.
        dl->AddRectFilled(a, b, withAlpha(IM_COL32(10, 13, 24, 255),
                          (unsigned)(220 * alpha)), 10.f);
        dl->AddRectFilledMultiColor(
            {a.x, a.y}, {b.x, b.y},
            withAlpha(it.color, (unsigned)(38 * alpha)),
            withAlpha(it.color, (unsigned)(10 * alpha)),
            withAlpha(it.color, (unsigned)(10 * alpha)),
            withAlpha(it.color, (unsigned)(38 * alpha)));
        dl->AddRect(a, b, withAlpha(it.color, (unsigned)(235 * alpha)),
                    10.f, 0, 1.8f);
        // Accent bars left + right of the text.
        dl->AddRectFilled({a.x + 14.f, cy - 1.f}, {a.x + 52.f, cy + 1.f},
            withAlpha(it.color, (unsigned)(220 * alpha)), 1.f);
        dl->AddRectFilled({b.x - 52.f, cy - 1.f}, {b.x - 14.f, cy + 1.f},
            withAlpha(it.color, (unsigned)(220 * alpha)), 1.f);
        // Big phase text, centred.
        ImFont* font = ImGui::GetFont();
        float fsz = 30.f;
        ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.f, it.text);
        ImVec2 tp { cx - ts.x * 0.5f, cy - ts.y * 0.5f };
        dl->AddText(font, fsz, {tp.x + 2.f, tp.y + 2.f},
            withAlpha(IM_COL32(0,0,0,255), (unsigned)(180 * alpha)), it.text);
        dl->AddText(font, fsz, tp,
            withAlpha(IM_COL32(248, 240, 220, 255),
                      (unsigned)(255 * alpha)), it.text);
    }

    // ── ChainPop: activation burst — gold ring + card name + CHAIN n badge ─
    void drawChainPop(ImDrawList* dl, const Anim& it, double t) {
        float a = 1.f - (float)t;
        ImVec2 c = it.a;   // absolute screen centre of the activating card
        // Burst ring snapping outward.
        float r = 18.f + 40.f * (float)t;
        dl->AddCircle(c, r, withAlpha(it.color, (unsigned)(230 * a)), 28,
                      3.0f * a + 0.6f);
        dl->AddCircleFilled(c, 10.f * (1.f - (float)t) + 3.f,
                            withAlpha(it.color, (unsigned)(120 * a)), 20);
        // "CHAIN n" badge — small rounded plate above the card, rising.
        float rise = 10.f + 26.f * (float)t;
        ImFont* font = ImGui::GetFont();
        if (it.ivalue > 0) {
            char badge[20];
            std::snprintf(badge, sizeof(badge), "CHAIN %d", it.ivalue);
            float fsz = 15.f;
            ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.f, badge);
            ImVec2 bc { c.x, c.y - rise - 30.f };
            ImVec2 p0 { bc.x - ts.x * 0.5f - 8.f, bc.y - ts.y * 0.5f - 3.f };
            ImVec2 p1 { bc.x + ts.x * 0.5f + 8.f, bc.y + ts.y * 0.5f + 3.f };
            dl->AddRectFilled(p0, p1, withAlpha(IM_COL32(14, 12, 24, 255),
                              (unsigned)(235 * a)), 5.f);
            dl->AddRect(p0, p1, withAlpha(it.color, (unsigned)(245 * a)),
                        5.f, 0, 1.6f);
            dl->AddText(font, fsz, {bc.x - ts.x * 0.5f, bc.y - ts.y * 0.5f},
                        withAlpha(it.color, (unsigned)(255 * a)), badge);
        }
        // Card name below the badge.
        if (it.text[0]) {
            float fsz = 17.f;
            ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.f, it.text);
            ImVec2 tp { c.x - ts.x * 0.5f, c.y - rise - 12.f };
            dl->AddText(font, fsz, {tp.x + 1.5f, tp.y + 1.5f},
                withAlpha(IM_COL32(0,0,0,255), (unsigned)(190 * a)), it.text);
            dl->AddText(font, fsz, tp,
                withAlpha(IM_COL32(250, 240, 220, 255),
                          (unsigned)(255 * a)), it.text);
            // Intent verb under the name (e.g. "Negate & Destroy").
            if (it.text2[0]) {
                float vs = 13.f;
                ImVec2 vts = font->CalcTextSizeA(vs, FLT_MAX, 0.f, it.text2);
                ImVec2 vp { c.x - vts.x * 0.5f, tp.y + ts.y + 1.f };
                dl->AddText(font, vs, {vp.x + 1.f, vp.y + 1.f},
                    withAlpha(IM_COL32(0,0,0,255), (unsigned)(170 * a)), it.text2);
                dl->AddText(font, vs, vp,
                    withAlpha(it.color, (unsigned)(255 * a)), it.text2);
            }
        }
    }

    // ── Reveal: searched card shown large, then shrinking toward the hand ──
    void drawReveal(ImDrawList* dl, const Anim& it, double t) {
        // 0–.45 grow+hold at source, .45–1 shrink + glide to destination.
        float a, scale; ImVec2 c = it.a;
        if (t < 0.45) { a = (t < 0.12) ? (float)(t / 0.12) : 1.f; scale = 1.f; }
        else {
            float k = (float)((t - 0.45) / 0.55);
            a = 1.f - k * 0.9f; scale = 1.f - 0.7f * k;
            c.x += (it.b.x - it.a.x) * k; c.y += (it.b.y - it.a.y) * k;
        }
        float ch = 150.f * scale, cw = ch * (421.f / 614.f);
        ImVec2 p0 { c.x - cw * 0.5f, c.y - ch * 0.5f };
        ImVec2 p1 { c.x + cw * 0.5f, c.y + ch * 0.5f };
        for (int g = 3; g >= 1; --g)
            dl->AddRectFilled({p0.x - g*5.f, p0.y - g*5.f},
                {p1.x + g*5.f, p1.y + g*5.f},
                withAlpha(it.color, (unsigned)((26 / g) * a)), 8.f);
        if (it.tex)
            dl->AddImageRounded((ImTextureID)it.tex, p0, p1, {0,0}, {1,1},
                withAlpha(IM_COL32_WHITE, (unsigned)(255 * a)), 5.f);
        else
            dl->AddRectFilled(p0, p1,
                withAlpha(IM_COL32(30, 40, 70, 255), (unsigned)(235 * a)), 5.f);
        dl->AddRect(p0, p1, withAlpha(it.color, (unsigned)(245 * a)), 5.f, 0, 2.f);
        if (it.text[0] && t < 0.5) {
            ImFont* font = ImGui::GetFont();
            float fsz = 16.f;
            ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.f, it.text);
            ImVec2 tp { c.x - ts.x * 0.5f, p0.y - ts.y - 8.f };
            dl->AddText(font, fsz, {tp.x + 1.5f, tp.y + 1.5f},
                withAlpha(IM_COL32(0,0,0,255), (unsigned)(180 * a)), it.text);
            dl->AddText(font, fsz, tp,
                withAlpha(IM_COL32(170, 220, 255, 255), (unsigned)(255 * a)),
                it.text);
        }
    }

    // ── Negate: shatter shards + red ✕ + "NEGATED" ─────────────────────────
    void drawNegate(ImDrawList* dl, const Anim& it, double t) {
        float a = 1.f - (float)t;
        ImVec2 c = it.a;
        // Shards flying outward.
        for (int s = 0; s < 10; ++s) {
            float ang = (float)s / 10.f * 6.2831853f + 0.3f;
            float rr = 8.f + 46.f * (float)t;
            ImVec2 p { c.x + std::cos(ang) * rr, c.y + std::sin(ang) * rr };
            ImVec2 d { p.x + std::cos(ang) * 8.f, p.y + std::sin(ang) * 8.f };
            dl->AddLine(p, d, withAlpha(it.color, (unsigned)(220 * a)), 2.2f);
        }
        // Red ✕.
        float x = 22.f;
        dl->AddLine({c.x - x, c.y - x}, {c.x + x, c.y + x},
                    withAlpha(it.color, (unsigned)(255 * a)), 4.0f);
        dl->AddLine({c.x + x, c.y - x}, {c.x - x, c.y + x},
                    withAlpha(it.color, (unsigned)(255 * a)), 4.0f);
        // "NEGATED" label.
        ImFont* font = ImGui::GetFont();
        float fsz = 18.f;
        const char* lab = "NEGATED";
        ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.f, lab);
        ImVec2 tp { c.x - ts.x * 0.5f, c.y + x + 6.f };
        dl->AddText(font, fsz, {tp.x + 1.5f, tp.y + 1.5f},
            withAlpha(IM_COL32(0,0,0,255), (unsigned)(190 * a)), lab);
        dl->AddText(font, fsz, tp,
            withAlpha(IM_COL32(255, 120, 120, 255), (unsigned)(255 * a)), lab);
    }

    // ── BossCard: dim field + energy rings + enlarged card + type label ───
    void drawBoss(ImDrawList* dl, ImVec2 winTL, const Anim& it, double t) {
        float W = it.a.x, H = it.a.y;
        // Envelope: 0-.18 rise+grow, .18-.62 hold, .62-1 settle toward zone.
        float alpha, scale;
        ImVec2 center { winTL.x + W * 0.5f, winTL.y + H * 0.46f };
        if (t < 0.18)      { float k = (float)(t / 0.18); alpha = k; scale = 0.6f + 0.4f * k; }
        else if (t < 0.62) { alpha = 1.f; scale = 1.0f; }
        else {
            float k = (float)((t - 0.62) / 0.38);
            alpha = 1.f - k * 0.85f;
            scale = 1.0f - 0.45f * k;
            // Drift toward the destination zone as it shrinks.
            center.x += (it.b.x - center.x) * k * 0.8f;
            center.y += (it.b.y - center.y) * k * 0.8f;
        }
        if (alpha < 0.f) alpha = 0.f;
        // Field dim.
        dl->AddRectFilled({winTL.x, winTL.y}, {winTL.x + W, winTL.y + H},
            withAlpha(IM_COL32(2, 4, 10, 255), (unsigned)(150 * alpha)), 0.f);
        // Energy rings expanding behind the card.
        for (int r = 0; r < 4; ++r) {
            float rad = (70.f + r * 46.f) * (0.7f + 0.5f * (float)t);
            dl->AddCircle(center, rad,
                withAlpha(it.color, (unsigned)((90 - r * 18) * alpha)),
                48, 2.2f);
        }
        // Radiating particle dots.
        for (int p = 0; p < 16; ++p) {
            float ang = (float)p / 16.f * 6.2831853f + (float)t * 1.4f;
            float rr  = 120.f + 90.f * (float)t;
            ImVec2 dp { center.x + std::cos(ang) * rr,
                        center.y + std::sin(ang) * rr };
            dl->AddCircleFilled(dp, 2.4f,
                withAlpha(it.color, (unsigned)(180 * alpha)), 8);
        }
        // Enlarged card (aspect 421:614).
        float ch = H * 0.52f * scale;
        float cw = ch * (421.f / 614.f);
        ImVec2 p0 { center.x - cw * 0.5f, center.y - ch * 0.5f };
        ImVec2 p1 { center.x + cw * 0.5f, center.y + ch * 0.5f };
        // Glow halo behind the card.
        for (int g = 3; g >= 1; --g) {
            float e = (float)g * 6.f;
            dl->AddRectFilled({p0.x - e, p0.y - e}, {p1.x + e, p1.y + e},
                withAlpha(it.color, (unsigned)((28 / g) * alpha)), 8.f + e);
        }
        dl->AddRectFilled({p0.x + 4.f, p0.y + 6.f}, {p1.x + 4.f, p1.y + 6.f},
            withAlpha(IM_COL32(0,0,0,255), (unsigned)(150 * alpha)), 6.f);
        if (it.tex)
            dl->AddImageRounded((ImTextureID)it.tex, p0, p1, {0,0}, {1,1},
                withAlpha(IM_COL32_WHITE, (unsigned)(255 * alpha)), 5.f);
        else
            dl->AddRectFilled(p0, p1,
                withAlpha(IM_COL32(30, 40, 70, 255),
                          (unsigned)(235 * alpha)), 5.f);
        dl->AddRect(p0, p1, withAlpha(it.color, (unsigned)(245 * alpha)),
                    5.f, 0, 2.4f);
        // Summon-type label above the card.
        if (it.text[0]) {
            ImFont* font = ImGui::GetFont();
            float fsz = 26.f;
            ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.f, it.text);
            ImVec2 tp { center.x - ts.x * 0.5f, p0.y - ts.y - 12.f };
            dl->AddText(font, fsz, {tp.x + 2.f, tp.y + 2.f},
                withAlpha(IM_COL32(0,0,0,255), (unsigned)(180 * alpha)),
                it.text);
            dl->AddText(font, fsz, tp,
                withAlpha(it.color, (unsigned)(255 * alpha)), it.text);
        }
    }

    AnimConfig        m_cfg;
    double            m_shakeStart = -1.0;
    double            m_shakeDur   = 0.3;
    float             m_shakeMag   = 7.f;
    std::vector<Anim> m_items;
};

} // namespace edo
