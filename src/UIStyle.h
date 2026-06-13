#pragma once
#include "imgui.h"
#include <string>

// Custom UI layer — replaces raw ImGui controls in the chrome layers (menu,
// setup modal, deck builder, duel HUD) with consistently-drawn pieces so the
// app stops looking like a default ImGui debug utility.
//
// Two parts:
//   * tokens      — palette / spacing / radii / typography
//   * draw helpers — DrawPanel, PrimaryButton, SecondaryButton, StatusChip,
//                    SectionHeader, ModalBackdrop, etc.
//
// Helpers live in this namespace (not the UI class) so any file in the project
// can use them without pulling in UI.h's heavy includes.

namespace UIStyle {

// ── Palette tokens ───────────────────────────────────────────────────────────
struct Colors {
    ImU32 bgDeep, bgPanel, bgRaised, bgPopup, bgGlass;
    ImU32 border, borderSoft;
    ImU32 accent, accentHi, accentDim, accentText;
    ImU32 primary, primaryHi, primaryAct, primaryText;
    ImU32 danger, success, warning;
    ImU32 textHi, textMd, textLo, textMuted;
    ImU32 shadow;
    ImU32 glowGold, glowCyan, glowGreen, glowMagenta;
};
const Colors& C();

// ── Spacing / radius tokens ──────────────────────────────────────────────────
struct Metrics {
    float radS, radM, radL;
    float padS, padM, padL;
    float btnH, btnHPrimary, btnHSmall;
    float rowH;
    float chipH;
    float sectionGap;
};
const Metrics& M();

// ── Fonts ────────────────────────────────────────────────────────────────────
// Set by Game::init after the font atlas is built. UI code uses Push/Pop calls
// below; if a font is null we transparently fall back to the default font.
extern ImFont* fTitle;   // ~32px display
extern ImFont* fHeader;  // ~22px section headers
extern ImFont* fBody;    // default body
extern ImFont* fSmall;   // ~13px metadata
void  PushFont(ImFont* f);
void  PopFont();

// ── Background / panel pieces (use ImDrawList directly) ─────────────────────
// Fullscreen subtle gradient backdrop used by the lobby / deck builder / duel
// background. Drawn to the foreground or background list depending on caller.
void DrawAppBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b);
// Dim the area behind a modal so it reads as elevated.
void DrawModalBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b);
// Card-like rounded panel: shadow + fill + 1px highlight border.
void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b,
               float rounding = 0.f, bool shadow = true);
// Sub-panel variant for inner sections (raised surface, lighter border).
void DrawRaisedPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding = 0.f);

// ── Headings / text helpers ─────────────────────────────────────────────────
void TitleText (const char* text);                  // big accent title
void Heading   (const char* text);                  // section heading
void Subtle    (const char* text);                  // muted line
void StatusChip(const char* label, ImU32 col);
// Section divider — title text on the left, hairline on the right.
void SectionHeader(const char* label);

// ── Buttons / interactive (custom-drawn) ────────────────────────────────────
// All buttons accept a `sz` where {x<=0,y<=0} fall back to available width and
// the metric default height. They return true on the same frame the user
// releases the mouse over them (matches ImGui::Button semantics).
bool PrimaryButton  (const char* label, ImVec2 sz = {0.f, 0.f});
bool SecondaryButton(const char* label, ImVec2 sz = {0.f, 0.f});
bool GhostButton    (const char* label, ImVec2 sz = {0.f, 0.f});
bool DangerButton   (const char* label, ImVec2 sz = {0.f, 0.f});
// Tonal switch — used for segmented phase buttons in the duel top bar.
bool SegmentedButton(const char* label, bool active, bool enabled,
                     ImVec2 sz = {0.f, 0.f});

} // namespace UIStyle
