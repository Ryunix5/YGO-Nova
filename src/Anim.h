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

namespace edo {

struct Anim {
    enum Type {
        Pulse,        // soft filled disc that grows + fades
        Ring,         // expanding ring outline
        Beam,         // line from a → b that fades
        LPFlash,      // rectangular flash on a player's LP panel
        DamageNum,    // floating "-NNN" number above origin
        ZoneFlash     // rectangular flash on a zone (placement / send to GY)
    };
    Type     type   = Pulse;
    double   start  = 0.0;       // ImGui::GetTime() seconds when queued
    double   dur    = 0.5;
    ImVec2   a {};               // primary point / rect top-left
    ImVec2   b {};               // beam end / rect bottom-right
    ImU32    color  = IM_COL32_WHITE;
    float    radius = 24.f;
    int      ivalue = 0;
    char     text[24] = {};
};

class AnimManager {
public:
    void clear()                  { m_items.clear(); }
    bool empty()         const    { return m_items.empty(); }
    size_t count()       const    { return m_items.size(); }

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
                float thick = 4.0f * a + 0.8f;
                dl->AddLine(it.a, it.b, col, thick);
                // Tiny pulse blob at the impact end.
                dl->AddCircleFilled(it.b, 6.0f * a + 2.0f,
                                    withAlpha(it.color, (unsigned)(180 * a)), 16);
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
            }
            ++i;
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
    void push(const Anim& a) { m_items.push_back(a); cap(); }

    std::vector<Anim> m_items;
};

} // namespace edo
