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

// ── Global theme ─────────────────────────────────────────────────────────────
// Writes the EdoPro+ palette into ImGui::GetStyle() so every RAW ImGui widget
// (combos, inputs, checkboxes, sliders, scrollbars, popup modals, tooltips,
// selectables, tabs) matches the custom-drawn chrome. Idempotent — call once
// at startup (UI::draw applies it lazily on the first frame).
void ApplyTheme();

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
// Translucent "glass" surface: tinted gradient fill + top sheen + hairline
// border. `tint` 0 uses the default glass colour. The duel HUD, overlays and
// floating panels share this so the whole app reads as one material.
void DrawGlassPanel(ImDrawList* dl, ImVec2 a, ImVec2 b,
                    float rounding = 0.f, ImU32 tint = 0);
// Layered soft glow ring around a rect — state accents (active phase pill,
// legal zone, primary button hover). `layers` controls spread (2-4 typical).
void DrawGlow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color,
              float rounding, int layers = 3);
// Horizontal hairline divider with vertical padding, layout-aware.
void DrawDivider(float padTop = 6.f, float padBot = 6.f);
// Premium OPAQUE game panel — deeper than glass, for solid menu surfaces
// (main-menu cards, setup cards). Drop shadow + gradient fill + top sheen +
// thin neon border + inner hairline. `accent` (0 = none) tints the border.
void DrawGamePanel(ImDrawList* dl, ImVec2 a, ImVec2 b,
                   float rounding = 0.f, ImU32 accent = 0);
// Gold-rimmed count pill centred on `center` (deck / GY / hand counters).
void CountBadge(ImDrawList* dl, ImVec2 center, int count, ImU32 accent = 0);
// Themed progress / fill bar inside [a,b] with sheen. `frac` 0..1.
void ProgressBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, ImU32 fill);

// ── Layout-cursor composite pieces ───────────────────────────────────────────
// Centred empty-state block (glyph + title + optional subtitle) filling a
// region of height `h`. Use inside viewers / lists when there's nothing to
// show, instead of a big blank box.
void EmptyState(float h, const char* title, const char* subtitle = nullptr);
// Small square ghost icon button (HUD corner actions). `active` tints gold.
bool IconButton(const char* label, ImVec2 sz, bool active = false);
// Themed single-line search input with a leading magnifier glyph. Returns
// true on edit. `width` <= 0 uses the available width.
bool SearchInput(const char* id, char* buf, size_t bufSz,
                 const char* hint, float width = -1.f);

// ── Composite interactive pieces ─────────────────────────────────────────────
// HUD pill — rounded chip-button used by phase bars and badge rows. Active
// state fills gold with a soft glow; inactive is a dark glass chip.
bool HudPill(const char* label, bool active, bool enabled,
             ImVec2 sz = {0.f, 0.f});
// Action card — a selectable row with a bright title and fully-wrapped
// description underneath (prompt/effect/chain choices). Height adapts to the
// wrapped text so long effect text is never cut off. Returns true on click;
// *outHovered reports hover for preview wiring.
bool ActionCard(const char* id, const char* title, const char* desc,
                float width, bool* outHovered = nullptr);

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
