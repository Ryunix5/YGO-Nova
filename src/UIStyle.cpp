#include "UIStyle.h"
#include "imgui_internal.h"
#include <cstring>
#include <cstdio>
#include <string>

namespace UIStyle {

// ── Token tables ─────────────────────────────────────────────────────────────
// Solid palette. Custom-drawn helpers below paint these via ImDrawList so
// nothing depends on the default ImGui look any more.
// Master Duel-inspired premium palette: deep navy/indigo surfaces, warm gold
// accent, cyan/blue highlights, restrained reds for damage. The whole app
// reads these tokens via UIStyle::C() so palette tweaks are one-stop.
// ── Red & Black theme ────────────────────────────────────────────────────────
// Deep near-black surfaces with a faint red bias, crimson accents, and a vivid
// red hero/primary. One change here reskins the whole app (every screen reads
// UIStyle::C()).
static const Colors gC = {
    /*bgDeep*/    IM_COL32( 10,   7,   8, 255),   // app background (near-black)
    /*bgPanel*/   IM_COL32( 22,  14,  16, 255),   // panel surface
    /*bgRaised*/  IM_COL32( 36,  20,  24, 255),   // input / hover surface
    /*bgPopup*/   IM_COL32( 17,  10,  12, 252),   // popup / modal background
    /*bgGlass*/   IM_COL32( 32,  16,  20, 222),   // tinted glass tile
    /*border*/    IM_COL32(116,  46,  52, 200),   // panel borders (muted crimson)
    /*borderSoft*/IM_COL32( 76,  34,  40, 140),   // inner section dividers
    /*accent*/    IM_COL32(198,  54,  60, 255),   // crimson red (a touch deeper)
    /*accentHi*/  IM_COL32(238,  86,  90, 255),   // red highlight
    /*accentDim*/ IM_COL32(110,  34,  38, 200),   // red dim
    /*accentText*/IM_COL32(255, 242, 242, 255),   // text on red buttons
    /*primary*/   IM_COL32(198,  30,  38, 255),   // vivid red primary (hero)
    /*primaryHi*/ IM_COL32(230,  56,  62, 255),
    /*primaryAct*/IM_COL32(252,  78,  84, 255),
    /*primaryText*/IM_COL32(255, 244, 244, 255),
    /*danger*/    IM_COL32(244,  86,  80, 255),
    /*success*/   IM_COL32( 92, 206, 126, 255),
    /*warning*/   IM_COL32(236, 184,  72, 255),
    /*textHi*/    IM_COL32(247, 240, 241, 255),
    /*textMd*/    IM_COL32(214, 190, 194, 255),
    /*textLo*/    IM_COL32(166, 130, 136, 255),
    /*textMuted*/ IM_COL32(120,  92,  98, 255),
    /*shadow*/    IM_COL32(  0,   0,   0, 150),
    /*glowGold*/  IM_COL32(255,  96,  96, 232),   // red glow (repurposed)
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

// ── Global theme ─────────────────────────────────────────────────────────────
// One write into ImGuiStyle covers every raw widget the custom helpers don't
// reach: combos, text inputs, checkboxes, sliders, scrollbars, selectables,
// popup modals, tooltips. Without this the Settings/Audio/Assets popups and
// the deck-builder inputs kept the stock ImGui grey-blue look and the app
// read as two different products.
void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    auto V4 = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };

    // Metrics — softer rounding + more breathing room for a premium feel.
    s.WindowRounding    = 13.f;
    s.ChildRounding     = 10.f;
    s.PopupRounding     = 13.f;
    s.FrameRounding     = 9.f;
    s.GrabRounding      = 8.f;
    s.TabRounding       = 8.f;
    s.ScrollbarRounding = 9.f;
    s.ScrollbarSize     = 13.f;
    s.WindowBorderSize  = 1.f;
    s.PopupBorderSize   = 1.f;
    s.FrameBorderSize   = 0.f;
    s.FramePadding      = {12.f, 8.f};
    s.ItemSpacing       = {9.f, 8.f};
    s.ItemInnerSpacing  = {7.f, 5.f};
    s.WindowPadding     = {16.f, 14.f};

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = V4(gC.textHi);
    c[ImGuiCol_TextDisabled]         = V4(gC.textLo);
    c[ImGuiCol_WindowBg]             = V4(gC.bgPanel);
    c[ImGuiCol_ChildBg]              = {0.f, 0.f, 0.f, 0.f};
    c[ImGuiCol_PopupBg]              = V4(gC.bgPopup);
    c[ImGuiCol_Border]               = V4(gC.borderSoft);
    c[ImGuiCol_BorderShadow]         = {0.f, 0.f, 0.f, 0.f};
    c[ImGuiCol_FrameBg]              = V4(IM_COL32( 32,  18,  22, 255));
    c[ImGuiCol_FrameBgHovered]       = V4(IM_COL32( 46,  24,  30, 255));
    c[ImGuiCol_FrameBgActive]        = V4(IM_COL32( 60,  30,  36, 255));
    c[ImGuiCol_TitleBg]              = V4(IM_COL32( 16,  10,  12, 255));
    c[ImGuiCol_TitleBgActive]        = V4(IM_COL32( 30,  15,  18, 255));
    c[ImGuiCol_TitleBgCollapsed]     = V4(IM_COL32( 16,  10,  12, 200));
    c[ImGuiCol_MenuBarBg]            = V4(IM_COL32( 18,  11,  13, 255));
    c[ImGuiCol_ScrollbarBg]          = V4(IM_COL32( 12,   7,   9, 160));
    c[ImGuiCol_ScrollbarGrab]        = V4(IM_COL32( 92,  42,  48, 200));
    c[ImGuiCol_ScrollbarGrabHovered] = V4(IM_COL32(124,  54,  60, 230));
    c[ImGuiCol_ScrollbarGrabActive]  = V4(IM_COL32(164,  66,  72, 255));
    c[ImGuiCol_CheckMark]            = V4(gC.accentHi);
    c[ImGuiCol_SliderGrab]           = V4(gC.accent);
    c[ImGuiCol_SliderGrabActive]     = V4(gC.accentHi);
    c[ImGuiCol_Button]               = V4(IM_COL32( 48,  24,  30, 255));
    c[ImGuiCol_ButtonHovered]        = V4(IM_COL32( 66,  32,  38, 255));
    c[ImGuiCol_ButtonActive]         = V4(IM_COL32( 84,  38,  46, 255));
    c[ImGuiCol_Header]               = V4(IM_COL32( 54,  26,  32, 220));
    c[ImGuiCol_HeaderHovered]        = V4(IM_COL32( 70,  32,  40, 235));
    c[ImGuiCol_HeaderActive]         = V4(IM_COL32( 88,  40,  48, 255));
    c[ImGuiCol_Separator]            = V4(gC.borderSoft);
    c[ImGuiCol_SeparatorHovered]     = V4(gC.border);
    c[ImGuiCol_SeparatorActive]      = V4(gC.accent);
    c[ImGuiCol_ResizeGrip]           = V4(IM_COL32( 92,  42,  48, 120));
    c[ImGuiCol_ResizeGripHovered]    = V4(IM_COL32(124,  54,  60, 180));
    c[ImGuiCol_ResizeGripActive]     = V4(gC.accent);
    c[ImGuiCol_Tab]                  = V4(IM_COL32( 32,  18,  22, 255));
    c[ImGuiCol_TabHovered]           = V4(IM_COL32( 66,  32,  38, 255));
    c[ImGuiCol_TabActive]            = V4(IM_COL32( 60,  30,  36, 255));
    c[ImGuiCol_TabDimmed]            = V4(IM_COL32( 22,  13,  16, 255));
    c[ImGuiCol_TabDimmedSelected]    = V4(IM_COL32( 40,  20,  24, 255));
    c[ImGuiCol_TextSelectedBg]       = V4(IM_COL32(170,  52,  58, 110));
    c[ImGuiCol_NavCursor]            = V4(gC.accent);
    c[ImGuiCol_ModalWindowDimBg]     = {0.02f, 0.03f, 0.06f, 0.62f};
}

// ── Panels ───────────────────────────────────────────────────────────────────
void DrawAppBackdrop(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    // Vertical gradient dark-red → near black + a soft crimson centre vignette
    // so empty space reads as deliberate, not a flat canvas.
    dl->AddRectFilledMultiColor(a, b,
        IM_COL32( 26,  12,  15, 255),     // tl
        IM_COL32( 30,  13,  16, 255),     // tr
        IM_COL32(  8,   4,   6, 255),     // br
        IM_COL32(  6,   3,   4, 255));    // bl
    const float cx = (a.x + b.x) * 0.5f;
    const float cy = (a.y + b.y) * 0.55f;
    const float r  = (b.x - a.x) * 0.55f;
    dl->AddCircleFilled({cx, cy}, r, IM_COL32(140, 30, 34, 24), 96);
    dl->AddCircleFilled({cx, cy}, r * 0.6f, IM_COL32(180, 44, 48, 18), 96);
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

void DrawGlassPanel(ImDrawList* dl, ImVec2 a, ImVec2 b,
                    float rounding, ImU32 tint) {
    if (rounding <= 0.f) rounding = gM.radM;
    if (tint == 0) tint = gC.bgGlass;
    // Soft drop shadow so the glass reads as floating.
    for (int i = 3; i >= 1; --i) {
        float e = (float)i * 1.6f;
        dl->AddRectFilled({a.x - e, a.y - e + e * 0.6f},
                          {b.x + e, b.y + e},
                          IM_COL32(0, 0, 0, 26), rounding + e);
    }
    dl->AddRectFilled(a, b, tint, rounding);
    // Top sheen — the "glass" cue.
    float h = b.y - a.y;
    dl->AddRectFilledMultiColor(a, {b.x, a.y + h * 0.30f},
        IM_COL32(255, 255, 255, 16), IM_COL32(255, 255, 255, 16),
        IM_COL32(255, 255, 255,  0), IM_COL32(255, 255, 255,  0));
    dl->AddRect(a, b, gC.border, rounding, 0, 1.2f);
    // Inner hairline for depth.
    dl->AddRect({a.x + 1.5f, a.y + 1.5f}, {b.x - 1.5f, b.y - 1.5f},
                IM_COL32(255, 255, 255, 14), rounding - 1.f, 0, 1.f);
}

void DrawGlow(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color,
              float rounding, int layers) {
    if (layers < 1) layers = 1;
    int baseA = (int)(color >> 24) & 0xFF;
    for (int i = layers; i >= 1; --i) {
        float e = (float)i * 2.6f;
        int alpha = baseA / (i * 3 + 2);
        ImU32 c = (color & 0x00FFFFFF) | ((ImU32)alpha << 24);
        dl->AddRectFilled({a.x - e, a.y - e}, {b.x + e, b.y + e},
                          c, rounding + e);
    }
}

void DrawDivider(float padTop, float padBot) {
    ImGui::Dummy({1.f, padTop});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddLine(p, {p.x + w, p.y}, gC.borderSoft, 1.f);
    ImGui::Dummy({1.f, padBot + 1.f});
}

// Forward declaration — visibleLabel is defined further down; IconButton
// (below) needs it before that point.
static const char* visibleLabel(const char* label);

void DrawGamePanel(ImDrawList* dl, ImVec2 a, ImVec2 b,
                   float rounding, ImU32 accent) {
    if (rounding <= 0.f) rounding = gM.radL;
    // Drop shadow.
    for (int i = 5; i >= 1; --i) {
        float e = (float)i * 1.8f;
        dl->AddRectFilled({a.x - e, a.y - e + e * 0.7f},
                          {b.x + e, b.y + e},
                          IM_COL32(0, 0, 0, 20), rounding + e);
    }
    // Vertical gradient body (raised → deep) for a moulded surface.
    dl->AddRectFilledMultiColor(a, b,
        IM_COL32(40, 21, 25, 255), IM_COL32(40, 21, 25, 255),
        IM_COL32(22, 12, 15, 255), IM_COL32(22, 12, 15, 255));
    // Top sheen.
    float h = b.y - a.y;
    dl->AddRectFilledMultiColor(a, {b.x, a.y + h * 0.22f},
        IM_COL32(255, 255, 255, 20), IM_COL32(255, 255, 255, 20),
        IM_COL32(255, 255, 255, 0),  IM_COL32(255, 255, 255, 0));
    ImU32 border = accent ? accent : gC.border;
    dl->AddRect(a, b, border, rounding, 0, 1.4f);
    dl->AddRect({a.x + 1.5f, a.y + 1.5f}, {b.x - 1.5f, b.y - 1.5f},
                IM_COL32(255, 255, 255, 12), rounding - 1.f, 0, 1.f);
}

void CountBadge(ImDrawList* dl, ImVec2 center, int count, ImU32 accent) {
    if (accent == 0) accent = gC.accent;
    PushFont(fSmall);
    char b[12]; std::snprintf(b, sizeof(b), "%d", count);
    ImVec2 ts = ImGui::CalcTextSize(b);
    float hw = ts.x * 0.5f + 8.f, hh = ts.y * 0.5f + 3.f;
    ImVec2 p0{center.x - hw, center.y - hh}, p1{center.x + hw, center.y + hh};
    dl->AddRectFilled(p0, p1, IM_COL32(16, 9, 11, 228), hh);
    dl->AddRect(p0, p1, (accent & 0x00FFFFFF) | 0xC0000000, hh, 0, 1.f);
    dl->AddText({center.x - ts.x * 0.5f, center.y - ts.y * 0.5f},
                IM_COL32(255, 226, 226, 245), b);
    PopFont();
}

void ProgressBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, ImU32 fill) {
    if (frac < 0.f) frac = 0.f; if (frac > 1.f) frac = 1.f;
    float r = (b.y - a.y) * 0.5f;
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 150), r);
    if (frac > 0.f) {
        float fw = a.x + (b.x - a.x) * frac;
        dl->AddRectFilled(a, {fw, b.y}, fill, r);
        dl->AddRectFilledMultiColor(a, {fw, a.y + (b.y - a.y) * 0.5f},
            IM_COL32(255, 255, 255, 55), IM_COL32(255, 255, 255, 55),
            IM_COL32(255, 255, 255, 0),  IM_COL32(255, 255, 255, 0));
    }
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 24), r, 0, 1.f);
}

void EmptyState(float h, const char* title, const char* subtitle) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 c{p.x + w * 0.5f, p.y + h * 0.42f};
    // Subtle ring + slash glyph.
    dl->AddCircle(c, 16.f, IM_COL32(176, 86, 92, 110), 24, 1.8f);
    dl->AddLine({c.x - 9.f, c.y - 9.f}, {c.x + 9.f, c.y + 9.f},
                IM_COL32(176, 86, 92, 110), 1.8f);
    if (title && title[0]) {
        ImVec2 ts = ImGui::CalcTextSize(title);
        dl->AddText({c.x - ts.x * 0.5f, c.y + 26.f}, gC.textLo, title);
    }
    if (subtitle && subtitle[0]) {
        PushFont(fSmall);
        ImVec2 ss = ImGui::CalcTextSize(subtitle);
        dl->AddText({c.x - ss.x * 0.5f, c.y + 46.f}, gC.textMuted, subtitle);
        PopFont();
    }
    ImGui::Dummy({w, h});
}

bool IconButton(const char* label, ImVec2 sz, bool active) {
    if (sz.x <= 0.f) sz.x = 30.f;
    if (sz.y <= 0.f) sz.y = 30.f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, sz);
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br{pos.x + sz.x, pos.y + sz.y};
    ImU32 fill = active  ? gC.accent
              : hovered  ? IM_COL32(64, 32, 38, 255)
                         : IM_COL32(34, 18, 22, 230);
    dl->AddRectFilled(pos, br, fill, gM.radS);
    dl->AddRect(pos, br,
                active ? gC.accentHi : hovered ? gC.border : gC.borderSoft,
                gM.radS, 0, 1.f);
    const char* vis = visibleLabel(label);
    ImVec2 ts = ImGui::CalcTextSize(vis);
    dl->AddText({pos.x + (sz.x - ts.x) * 0.5f, pos.y + (sz.y - ts.y) * 0.5f},
                active ? gC.accentText : hovered ? gC.textHi : gC.textMd, vis);
    return clicked;
}

bool SearchInput(const char* id, char* buf, size_t bufSz,
                 const char* hint, float width) {
    if (width <= 0.f) width = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float h = gM.btnH;
    // Magnifier glyph at the left.
    ImVec2 gc{pos.x + 14.f, pos.y + h * 0.5f};
    dl->AddCircle(gc, 5.f, gC.textLo, 16, 1.4f);
    dl->AddLine({gc.x + 4.f, gc.y + 4.f}, {gc.x + 8.f, gc.y + 8.f},
                gC.textLo, 1.4f);
    // Inset the input field past the glyph.
    ImGui::SetCursorScreenPos({pos.x + 28.f, pos.y});
    ImGui::SetNextItemWidth(width - 28.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{8.f, 7.f});
    bool edited = ImGui::InputTextWithHint(id, hint, buf, bufSz);
    ImGui::PopStyleVar();
    return edited;
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
    float r = gM.radM;
    // Hover glow ring for a premium "lit" feel.
    if (hovered && !active)
        DrawGlow(dl, a, b, (gC.accentHi & 0x00FFFFFF) | 0x66000000, r, 3);
    // Drop shadow below for depth.
    dl->AddRectFilled({a.x + 1.f, a.y + 3.f}, {b.x + 1.f, b.y + 4.f},
                      IM_COL32(0, 0, 0, 70), r);
    // Vertical gradient body: bright crimson top → deep red bottom (press
    // inverts slightly for a tactile "pushed in" cue).
    ImU32 top = active ? gC.primary   : hovered ? gC.primaryAct : gC.primaryHi;
    ImU32 bot = active ? gC.primaryHi : hovered ? gC.primary    : gC.primary;
    dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
    // Crisp top sheen.
    float h = b.y - a.y;
    dl->AddRectFilledMultiColor(
        {a.x, a.y}, {b.x, a.y + h * 0.46f},
        IM_COL32(255, 255, 255, 52), IM_COL32(255, 255, 255, 52),
        IM_COL32(255, 255, 255,  0), IM_COL32(255, 255, 255,  0));
    // Hairline inner highlight + bright outer border.
    dl->AddRect({a.x + 1.f, a.y + 1.f}, {b.x - 1.f, b.y - 1.f},
                IM_COL32(255, 255, 255, 30), r - 1.f, 0, 1.f);
    dl->AddRect(a, b, hovered ? IM_COL32(255, 140, 140, 255)
                              : IM_COL32(255, 110, 110, 220),
                r, 0, hovered ? 1.6f : 1.2f);
    drawButtonLabel(dl, a, b, gC.primaryText, label);
}

static void paintSecondary(ImDrawList* dl, ImVec2 a, ImVec2 b,
                           bool hovered, bool active, const char* label) {
    ImU32 fill = active  ? IM_COL32(70, 34, 40, 255)
              : hovered  ? IM_COL32(62, 30, 36, 255)
                         : IM_COL32(46, 24, 28, 255);
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
              : hovered ? IM_COL32(66, 32, 38, 255)
                        : IM_COL32(36, 20, 24, 255);
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

bool HudPill(const char* label, bool active, bool enabled, ImVec2 sz) {
    const char* vis = visibleLabel(label);
    PushFont(fSmall);
    ImVec2 ts = ImGui::CalcTextSize(vis);
    PopFont();
    if (sz.x <= 0.f) sz.x = ts.x + 26.f;
    if (sz.y <= 0.f) sz.y = 28.f;
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImVec2 b = {a.x + sz.x, a.y + sz.y};
    float  r = sz.y * 0.30f;
    if (!enabled) ImGui::BeginDisabled();
    ImGui::InvisibleButton(label, sz);
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active) DrawGlow(dl, a, b, gC.glowGold, r, 3);
    ImU32 fill = active  ? gC.accent
              : hovered ? IM_COL32(54, 27, 32, 240)
                        : IM_COL32(28, 15, 18, 230);
    dl->AddRectFilled(a, b, fill, r);
    dl->AddRectFilledMultiColor(a, {b.x, a.y + sz.y * 0.5f},
        IM_COL32(255,255,255, active ? 38 : 16),
        IM_COL32(255,255,255, active ? 38 : 16),
        IM_COL32(255,255,255, 0), IM_COL32(255,255,255, 0));
    dl->AddRect(a, b,
                active  ? gC.accentHi
              : hovered ? gC.border
                        : IM_COL32(120, 56, 62, 180),
                r, 0, active ? 1.6f : 1.f);
    PushFont(fSmall);
    ImVec2 lts = ImGui::CalcTextSize(vis);
    dl->AddText({a.x + (sz.x - lts.x) * 0.5f, a.y + (sz.y - lts.y) * 0.5f},
                active  ? gC.accentText
              : enabled ? gC.textMd
                        : gC.textMuted, vis);
    PopFont();
    if (!enabled) ImGui::EndDisabled();
    return enabled && clicked;
}

bool ActionCard(const char* id, const char* title, const char* desc,
                float width, bool* outHovered) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    const float padX = 12.f, padY = 9.f, gap = 4.f;
    float innerW = width - 2.f * padX;
    if (innerW < 60.f) innerW = 60.f;
    bool hasDesc = desc && desc[0];
    ImVec2 tSz = ImGui::CalcTextSize(title, nullptr, false, innerW);
    ImVec2 dSz = hasDesc
        ? ImGui::CalcTextSize(desc, nullptr, false, innerW)
        : ImVec2{0.f, 0.f};
    float h = padY * 2.f + tSz.y + (hasDesc ? (gap + dSz.y) : 0.f);
    if (h < 40.f) h = 40.f;

    bool clicked = ImGui::InvisibleButton(id, {width, h});
    bool hov     = ImGui::IsItemHovered();
    bool held    = ImGui::IsItemActive();
    if (outHovered) *outHovered = hov;

    ImVec2 br = {pos.x + width, pos.y + h};
    if (hov) DrawGlow(dl, pos, br, gC.glowGold, 7.f, 2);
    ImU32 bg = held ? IM_COL32(86, 38, 44, 255)
             : hov  ? IM_COL32(70, 32, 38, 255)
                    : IM_COL32(44, 22, 26, 255);
    ImU32 border = hov ? gC.accentHi : IM_COL32(124, 56, 62, 190);
    dl->AddRectFilled(pos, br, bg, 7.f);
    dl->AddRect(pos, br, border, 7.f, 0, hov ? 1.8f : 1.f);
    // Left accent bar so rows read as selectable action cards.
    dl->AddRectFilled({pos.x, pos.y + 6.f}, {pos.x + 3.f, pos.y + h - 6.f},
                      hov ? gC.accentHi : IM_COL32(168, 76, 82, 200), 2.f);
    dl->AddText(nullptr, 0.f, {pos.x + padX, pos.y + padY},
                IM_COL32(255, 218, 200, 255), title, nullptr, innerW);
    if (hasDesc)
        dl->AddText(nullptr, 0.f,
                    {pos.x + padX, pos.y + padY + tSz.y + gap},
                    IM_COL32(220, 200, 204, 255), desc, nullptr, innerW);
    ImGui::Dummy({1.f, 6.f});                 // inter-row gap
    return clicked;
}

} // namespace UIStyle
