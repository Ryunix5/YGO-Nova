#include "UIStyle.h"
#include "imgui_internal.h"
#include <cstring>
#include <string>

namespace UIStyle {

// ── Token tables ─────────────────────────────────────────────────────────────
// Solid palette. Custom-drawn helpers below paint these via ImDrawList so
// nothing depends on the default ImGui look any more.
// Master Duel-inspired premium palette: deep navy/indigo surfaces, warm gold
// accent, cyan/blue highlights, restrained reds for damage. The whole app
// reads these tokens via UIStyle::C() so palette tweaks are one-stop.
static const Colors gC = {
    /*bgDeep*/    IM_COL32(  6,   9,  18, 255),   // app background (near-black navy)
    /*bgPanel*/   IM_COL32( 17,  22,  36, 255),   // panel surface
    /*bgRaised*/  IM_COL32( 26,  34,  56, 255),   // input / hover surface
    /*bgPopup*/   IM_COL32( 13,  18,  32, 252),   // popup / modal background
    /*bgGlass*/   IM_COL32( 22,  30,  52, 220),   // tinted glass tile
    /*border*/    IM_COL32( 70,  90, 130, 220),   // visible panel borders
    /*borderSoft*/IM_COL32( 48,  62,  92, 145),   // inner section dividers
    /*accent*/    IM_COL32(232, 182,  72, 255),   // warm gold
    /*accentHi*/  IM_COL32(255, 214, 108, 255),   // gold highlight
    /*accentDim*/ IM_COL32(126, 100,  40, 200),   // gold dim
    /*accentText*/IM_COL32( 22,  16,   6, 255),   // text on gold buttons
    /*primary*/   IM_COL32( 48, 110, 210, 255),   // cyan-blue primary
    /*primaryHi*/ IM_COL32( 76, 144, 240, 255),
    /*primaryAct*/IM_COL32( 96, 168, 252, 255),
    /*primaryText*/IM_COL32(232, 240, 252, 255),
    /*danger*/    IM_COL32(224,  82,  82, 255),
    /*success*/   IM_COL32( 90, 210, 130, 255),
    /*warning*/   IM_COL32(238, 182,  72, 255),
    /*textHi*/    IM_COL32(242, 246, 252, 255),
    /*textMd*/    IM_COL32(186, 198, 220, 255),
    /*textLo*/    IM_COL32(124, 140, 168, 255),
    /*textMuted*/ IM_COL32( 84, 100, 128, 255),
    /*shadow*/    IM_COL32(  0,   0,   0, 140),
    /*glowGold*/  IM_COL32(255, 214, 116, 232),
    /*glowCyan*/  IM_COL32(112, 220, 255, 232),
    /*glowGreen*/ IM_COL32(110, 240, 130, 232),
    /*glowMagenta*/IM_COL32(244, 132, 232, 232),
};
const Colors& C() { return gC; }

static const Metrics gM = {
    /*radS*/4.f, /*radM*/8.f, /*radL*/14.f,
    /*padS*/6.f, /*padM*/12.f, /*padL*/20.f,
    /*btnH*/34.f, /*btnHPrimary*/48.f, /*btnHSmall*/26.f,
    /*rowH*/40.f, /*chipH*/22.f,
    /*sectionGap*/18.f,
};
const Metrics& M() { return gM; }

ImFont* fTitle  = nullptr;
ImFont* fHeader = nullptr;
ImFont* fBody   = nullptr;
ImFont* fSmall  = nullptr;
void PushFont(ImFont* f) { if (f) ImGui::PushFont(f); }
void PopFont()           {        ImGui::PopFont(); }

// ── Panels ───────────────────────────────────────────────────────────────────
void DrawAppBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    // Vertical gradient navy → almost black + a soft warm centre vignette so
    // empty space stops looking like a flat XP canvas.
    dl->AddRectFilledMultiColor(a, b,
        IM_COL32( 18,  22,  34, 255),     // tl
        IM_COL32( 22,  26,  40, 255),     // tr
        IM_COL32(  6,   8,  14, 255),     // br
        IM_COL32(  4,   5,  10, 255));    // bl
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.55f;
    const float r  = (b.x - a.x) * 0.55f;
    dl->AddCircleFilled({cx, cy}, r, IM_COL32(80, 50, 14, 22), 96);
    dl->AddCircleFilled({cx, cy}, r * 0.6f, IM_COL32(120, 80, 30, 18), 96);
}

void DrawModalBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 130));
}

void DrawPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, bool shadow) {
    if (rounding <= 0.f) rounding = gM.radM;
    if (shadow) {
        // Soft drop shadow under the panel.
        for (int i = 4; i >= 1; --i) {
            float exp = (float)i;
            dl->AddRectFilled({a.x - exp, a.y - exp + exp},
                              {b.x + exp, b.y + exp + exp},
                              IM_COL32(0, 0, 0, 22), rounding + exp);
        }
    }
    dl->AddRectFilled(a, b, gC.bgPanel, rounding);
    // Inner highlight at top for a "lit from above" feel.
    dl->AddRectFilledMultiColor(a, {b.x, a.y + 28.f},
        IM_COL32(255, 255, 255, 14), IM_COL32(255, 255, 255, 14),
        IM_COL32(  0,   0,   0,  0), IM_COL32(  0,   0,   0,  0));
    dl->AddRect(a, b, gC.border, rounding, 0, 1.2f);
}

void DrawRaisedPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding) {
    if (rounding <= 0.f) rounding = gM.radS;
    dl->AddRectFilled(a, b, gC.bgRaised, rounding);
    dl->AddRect(a, b, gC.borderSoft, rounding, 0, 1.f);
}

// ── Heading / text helpers ──────────────────────────────────────────────────
void TitleText(const char* text) {
    PushFont(fTitle);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(gC.accent));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    PopFont();
}
void Heading(const char* text) {
    PushFont(fHeader);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(gC.textHi));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    PopFont();
}
void Subtle(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(gC.textLo));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// Strip the "##id" suffix from a label so the visible text matches ImGui's
// usual convention (everything from the first `##` is the unique-ID part).
// Returns a pointer to a thread-local trimmed copy. Defined here so the
// chip and the buttons below can both share it.
static const char* visibleLabel(const char* label);

void StatusChip(const char* label, ImU32 col) {
    PushFont(fSmall);
    const char* vis = visibleLabel(label);
    ImVec2 ts = ImGui::CalcTextSize(vis);
    ImVec2 padded = {ts.x + gM.padM, gM.chipH};
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(padded);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br = {pos.x + padded.x, pos.y + padded.y};
    dl->AddRectFilled(pos, br, (col & 0x00FFFFFF) | 0x33000000, gM.chipH * 0.5f);
    dl->AddRect      (pos, br, col, gM.chipH * 0.5f, 0, 1.f);
    dl->AddText({pos.x + gM.padM * 0.5f, pos.y + (padded.y - ts.y) * 0.5f},
                col, vis);
    PopFont();
}

void SectionHeader(const char* label) {
    PushFont(fHeader);
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImGui::TextUnformatted(label);
    PopFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetItemRectMin();
    ImVec2 b = ImGui::GetItemRectMax();
    // Hairline under-line on the right of the label fading out.
    float y = b.y - 2.f;
    dl->AddLine({b.x + 12.f, y},
                {a.x + ImGui::GetContentRegionAvail().x + b.x - a.x, y},
                gC.borderSoft, 1.f);
    (void)ts;
}

// ── Custom-drawn buttons ────────────────────────────────────────────────────
// Shared core: lays out an InvisibleButton of size `sz`, runs hit testing,
// then asks the caller-supplied painter to draw the visible button at the
// resolved screen rect. Returns whether the button was clicked this frame.
static bool ButtonCore(const char* label, ImVec2 sz, float defaultH,
                       void (*paint)(ImDrawList*, ImVec2, ImVec2,
                                     bool hovered, bool active,
                                     const char* label)) {
    if (sz.x <= 0.f) sz.x = ImGui::GetContentRegionAvail().x;
    if (sz.y <= 0.f) sz.y = defaultH;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, sz);
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    paint(dl, pos, {pos.x + sz.x, pos.y + sz.y}, hovered, active, label);
    return clicked;
}

// Strip the "##id" suffix from a label so the visible text matches ImGui's
// usual convention (everything from the first `##` is the unique-ID part).
// Returns a pointer to a thread-local trimmed copy.
static const char* visibleLabel(const char* label) {
    if (!label) return "";
    const char* hash = strstr(label, "##");
    if (!hash) return label;
    static thread_local char buf[256];
    size_t n = (size_t)(hash - label);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, label, n);
    buf[n] = '\0';
    return buf;
}

// Centred text on a custom button, honouring `##id`-stripping.
static void drawButtonLabel(ImDrawList* dl, ImVec2 a, ImVec2 b,
                            ImU32 col, const char* label) {
    const char* vis = visibleLabel(label);
    PushFont(fBody);
    ImVec2 ts = ImGui::CalcTextSize(vis);
    dl->AddText({a.x + (b.x - a.x - ts.x) * 0.5f,
                 a.y + (b.y - a.y - ts.y) * 0.5f},
                col, vis);
    PopFont();
}

static void paintPrimary(ImDrawList* dl, ImVec2 a, ImVec2 b,
                         bool hovered, bool active, const char* label) {
    // One clean solid gold — no offset shadow, no double-layer. State just
    // shifts the fill: brighter on hover, slightly dimmer on press.
    ImU32 fill = active  ? gC.accentDim
              : hovered  ? gC.accentHi
                         : gC.accent;
    dl->AddRectFilled(a, b, fill, gM.radM);
    // Subtle inner highlight at the top for a "lit from above" cue that
    // doesn't read as a shadow.
    float h = b.y - a.y;
    dl->AddRectFilledMultiColor(
        a, {b.x, a.y + h * 0.5f},
        IM_COL32(255, 255, 255, 38), IM_COL32(255, 255, 255, 38),
        IM_COL32(255, 255, 255,  0), IM_COL32(255, 255, 255,  0));
    dl->AddRect(a, b, hovered ? gC.accentHi : gC.accent,
                gM.radM, 0, hovered ? 1.4f : 1.f);
    drawButtonLabel(dl, a, b, gC.accentText, label);
}

static void paintSecondary(ImDrawList* dl, ImVec2 a, ImVec2 b,
                           bool hovered, bool active, const char* label) {
    ImU32 fill = active  ? IM_COL32(45, 60, 95, 255)
              : hovered  ? IM_COL32(40, 55, 88, 255)
                         : IM_COL32(30, 42, 70, 255);
    dl->AddRectFilled(a, b, fill, gM.radM);
    dl->AddRect(a, b, hovered ? gC.primaryHi : gC.border,
                gM.radM, 0, hovered ? 1.4f : 1.f);
    drawButtonLabel(dl, a, b, gC.textHi, label);
}

static void paintGhost(ImDrawList* dl, ImVec2 a, ImVec2 b,
                       bool hovered, bool active, const char* label) {
    ImU32 fill = active  ? IM_COL32(255, 255, 255, 22)
              : hovered  ? IM_COL32(255, 255, 255, 12)
                         : IM_COL32(0,   0,   0,   0);
    dl->AddRectFilled(a, b, fill, gM.radM);
    dl->AddRect(a, b, hovered ? gC.border : gC.borderSoft,
                gM.radM, 0, 1.f);
    drawButtonLabel(dl, a, b, hovered ? gC.textHi : gC.textMd, label);
}

static void paintDanger(ImDrawList* dl, ImVec2 a, ImVec2 b,
                        bool hovered, bool active, const char* label) {
    ImU32 fill = active  ? IM_COL32(190,  60,  60, 255)
              : hovered  ? IM_COL32(220,  82,  82, 255)
                         : IM_COL32(180,  64,  64, 255);
    dl->AddRectFilled(a, b, fill, gM.radM);
    dl->AddRect(a, b, IM_COL32(255, 120, 120, 220), gM.radM, 0, 1.2f);
    drawButtonLabel(dl, a, b, IM_COL32(255, 240, 240, 255), label);
}

bool PrimaryButton  (const char* l, ImVec2 sz) { return ButtonCore(l, sz, gM.btnHPrimary, paintPrimary); }
bool SecondaryButton(const char* l, ImVec2 sz) { return ButtonCore(l, sz, gM.btnH,        paintSecondary); }
bool GhostButton    (const char* l, ImVec2 sz) { return ButtonCore(l, sz, gM.btnH,        paintGhost); }
bool DangerButton   (const char* l, ImVec2 sz) { return ButtonCore(l, sz, gM.btnH,        paintDanger); }

bool SegmentedButton(const char* label, bool active, bool enabled, ImVec2 sz) {
    if (sz.x <= 0.f) sz.x = ImGui::GetContentRegionAvail().x;
    if (sz.y <= 0.f) sz.y = gM.btnHSmall;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    if (!enabled) ImGui::BeginDisabled();
    ImGui::InvisibleButton(label, sz);
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br = {pos.x + sz.x, pos.y + sz.y};
    ImU32 fill = active  ? gC.accent
              : hovered ? IM_COL32(45, 56, 82, 255)
                        : IM_COL32(28, 34, 50, 255);
    dl->AddRectFilled(pos, br, fill, gM.radS);
    dl->AddRect(pos, br,
                active ? gC.accentHi : gC.border,
                gM.radS, 0, active ? 1.4f : 1.f);
    PushFont(fSmall);
    const char* vis = visibleLabel(label);
    ImVec2 ts = ImGui::CalcTextSize(vis);
    dl->AddText({pos.x + (sz.x - ts.x) * 0.5f, pos.y + (sz.y - ts.y) * 0.5f},
                active        ? gC.accentText
              : enabled       ? gC.textMd
                              : gC.textMuted,
                vis);
    PopFont();
    if (!enabled) ImGui::EndDisabled();
    return enabled && clicked;
}

} // namespace UIStyle
