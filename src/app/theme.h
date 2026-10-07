#pragma once
#include <cmath>
#include <cstdio>
#include <string>
#include "imgui.h"
#include "imgui_internal.h"

// The visual system shared by Wardrobe and its installer.
//
// A deep, quiet, blue-black surface lit by one soft glow; depth comes from
// light and hairlines, not from boxes. One gradient accent (violet to azure)
// marks the single thing to do on a screen. Green, amber and red mean state
// and nothing else, so nothing merely clickable reads as a warning. Motion
// explains a change (a page arriving, a press, progress), it never decorates.
namespace wardrobe::ui {

namespace color {
constexpr ImU32 Background  = IM_COL32(0x09, 0x0B, 0x10, 0xFF);
constexpr ImU32 Surface     = IM_COL32(0x12, 0x15, 0x1D, 0xFF);
constexpr ImU32 SurfaceHigh = IM_COL32(0x19, 0x1D, 0x28, 0xFF);
constexpr ImU32 Raised      = IM_COL32(0x22, 0x27, 0x34, 0xFF);
constexpr ImU32 Hairline    = IM_COL32(0xFF, 0xFF, 0xFF, 0x12);
constexpr ImU32 HairlineHi  = IM_COL32(0xFF, 0xFF, 0xFF, 0x22);
constexpr ImU32 Text        = IM_COL32(0xF3, 0xF5, 0xFA, 0xFF);
constexpr ImU32 Muted       = IM_COL32(0xA6, 0xAE, 0xC1, 0xFF);
constexpr ImU32 Faint       = IM_COL32(0x6A, 0x72, 0x86, 0xFF);
constexpr ImU32 Violet      = IM_COL32(0x7C, 0x5C, 0xFF, 0xFF);
constexpr ImU32 Azure       = IM_COL32(0x4F, 0xB4, 0xFF, 0xFF);
constexpr ImU32 AccentText  = IM_COL32(0xB4, 0xA6, 0xFF, 0xFF);
constexpr ImU32 Ok          = IM_COL32(0x3D, 0xD6, 0x8C, 0xFF);
constexpr ImU32 Warn        = IM_COL32(0xF5, 0xB8, 0x41, 0xFF);
constexpr ImU32 Bad         = IM_COL32(0xFF, 0x6B, 0x6B, 0xFF);
constexpr ImU32 White       = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);

inline ImVec4 Vec(ImU32 packed) { return ImGui::ColorConvertU32ToFloat4(packed); }
inline ImU32 Fade(ImU32 packed, float alpha) {
    ImVec4 value = Vec(packed);
    value.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(value);
}
inline ImU32 Mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = Vec(a), y = Vec(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                                 x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}
}  // namespace color

namespace metric {
constexpr float CardRadius = 14.0f, ControlRadius = 10.0f;
constexpr float Gap = 10.0f, Pad = 20.0f, Section = 28.0f;
constexpr float TextTiny = 12.0f, TextSmall = 13.5f, TextBody = 15.0f, TextLead = 17.0f;
constexpr float TextTitle = 22.0f, TextDisplay = 34.0f;
}  // namespace metric

// Segoe Fluent Icons (Windows 11) and Segoe MDL2 Assets (Windows 10) share
// these code points, so the icons survive on either.
namespace icon {
inline constexpr const char* Home = u8"";
inline constexpr const char* Tools = u8"";
inline constexpr const char* Journal = u8"";
inline constexpr const char* Play = u8"";
inline constexpr const char* Refresh = u8"";
inline constexpr const char* Check = u8"";
inline constexpr const char* Warning = u8"";
inline constexpr const char* Error = u8"";
inline constexpr const char* Info = u8"";
inline constexpr const char* Download = u8"";
inline constexpr const char* Sync = u8"";
inline constexpr const char* Shield = u8"";
inline constexpr const char* Folder = u8"";
inline constexpr const char* Delete = u8"";
inline constexpr const char* Game = u8"";
inline constexpr const char* Library = u8"";
inline constexpr const char* Code = u8"";
inline constexpr const char* Package = u8"";
inline constexpr const char* Search = u8"";
inline constexpr const char* Clock = u8"";
inline constexpr const char* Close = u8"";
inline constexpr const char* Chevron = u8"";
inline constexpr const char* Hanger = u8"";
inline constexpr const char* Restore = u8"";
inline constexpr const char* Lightning = u8"";
inline constexpr const char* Erase = u8"";
inline constexpr const char* Desktop = u8"";
}  // namespace icon

struct Fonts {
    ImFont* text = nullptr;
    ImFont* strong = nullptr;
    ImFont* bold = nullptr;
    ImFont* display = nullptr;
    ImFont* icons = nullptr;
    ImFont* mono = nullptr;
};

inline Fonts& fonts() {
    static Fonts instance;
    return instance;
}

inline void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Flags |= ImFontAtlasFlags_NoMouseCursors;
    auto load = [&](std::initializer_list<const char*> files, float size) -> ImFont* {
        for (const char* file : files) {
            char path[MAX_PATH]{};
            if (!GetWindowsDirectoryA(path, MAX_PATH)) return nullptr;
            strcat_s(path, "\\Fonts\\");
            strcat_s(path, file);
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
            ImFontConfig config;
            config.OversampleH = 2;
            if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, size, &config)) return font;
        }
        return nullptr;
    };
    Fonts& f = fonts();
    f.text = load({"segoeui.ttf"}, metric::TextBody);
    f.strong = load({"seguisb.ttf", "segoeuib.ttf"}, metric::TextBody);
    f.bold = load({"segoeuib.ttf", "seguisb.ttf"}, metric::TextBody);
    f.display = load({"bahnschrift.ttf", "segoeuib.ttf"}, metric::TextDisplay);
    f.icons = load({"SegoeIcons.ttf", "segmdl2.ttf"}, metric::TextBody);
    f.mono = load({"CascadiaMono.ttf", "consola.ttf"}, metric::TextSmall);
    if (!f.text) f.text = io.Fonts->AddFontDefault();
    if (!f.strong) f.strong = f.text;
    if (!f.bold) f.bold = f.strong;
    if (!f.display) f.display = f.bold;
    if (!f.mono) f.mono = f.text;
    io.FontDefault = f.text;
}

inline void ApplyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(metric::Section, metric::Section);
    style.FramePadding = ImVec2(14, 9);
    style.ItemSpacing = ImVec2(metric::Gap, metric::Gap);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 8;
    style.WindowRounding = 0;
    style.ChildRounding = metric::CardRadius;
    style.FrameRounding = metric::ControlRadius;
    style.PopupRounding = metric::CardRadius;
    style.ScrollbarRounding = 8;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = color::Vec(color::Surface);
    c[ImGuiCol_ModalWindowDimBg] = color::Vec(IM_COL32(4, 5, 9, 190));
    c[ImGuiCol_Border] = color::Vec(color::HairlineHi);
    c[ImGuiCol_Text] = color::Vec(color::Text);
    c[ImGuiCol_TextDisabled] = color::Vec(color::Faint);
    c[ImGuiCol_FrameBg] = color::Vec(color::Raised);
    c[ImGuiCol_Button] = color::Vec(color::SurfaceHigh);
    c[ImGuiCol_ButtonHovered] = color::Vec(color::Raised);
    c[ImGuiCol_ButtonActive] = color::Vec(color::Raised);
    c[ImGuiCol_Separator] = color::Vec(color::Hairline);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = color::Vec(color::Raised);
    c[ImGuiCol_ScrollbarGrabHovered] = color::Vec(color::Faint);
    c[ImGuiCol_ScrollbarGrabActive] = color::Vec(color::Muted);
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);
}

// ---------------------------------------------------------------- motion
inline float Ease(float t) { t = ImClamp(t, 0.0f, 1.0f); return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }

// A value that glides towards its target, kept per widget id across frames.
inline float Approach(ImGuiID id, float target, float speed = 14.0f) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    float* value = storage->GetFloatRef(id, target);
    *value += (target - *value) * (1.0f - std::exp(-speed * ImGui::GetIO().DeltaTime));
    if (std::fabs(target - *value) < 0.001f) *value = target;
    return *value;
}

// ---------------------------------------------------------------- painting
// Recolours the vertices just added along p0 -> p1, opacity included (ImGui's
// own helper keeps the original alpha, which turned faint lines opaque).
inline void ShadeGradient(ImDrawList* draw, int start, ImVec2 p0, ImVec2 p1, ImU32 from, ImU32 to) {
    const ImVec2 axis(p1.x - p0.x, p1.y - p0.y);
    const float length = ImMax(axis.x * axis.x + axis.y * axis.y, 1e-4f);
    const ImVec4 a = color::Vec(from), b = color::Vec(to);
    for (int i = start; i < draw->VtxBuffer.Size; ++i) {
        ImDrawVert& v = draw->VtxBuffer[i];
        const float t = ImClamp(((v.pos.x - p0.x) * axis.x + (v.pos.y - p0.y) * axis.y) / length, 0.0f, 1.0f);
        const float alpha = ((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;   // keeps antialiasing fringes soft
        v.col = ImGui::ColorConvertFloat4ToU32(ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                                                      (a.w + (b.w - a.w) * t) * alpha));
    }
}
inline void GradientRect(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 from, ImU32 to, float rounding = 0,
                         ImDrawFlags flags = 0, ImVec2 p0 = ImVec2(-1, -1), ImVec2 p1 = ImVec2(-1, -1)) {
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, max, IM_COL32_WHITE, rounding, flags);
    if (p0.x < 0) { p0 = min; p1 = max; }
    ShadeGradient(draw, start, p0, p1, from, to);
}

// A soft light: full colour at the centre, fading to nothing at the radius.
inline void Glow(ImDrawList* draw, ImVec2 centre, float radius, ImU32 tint, int segments = 64) {
    const ImU32 edge = tint & ~IM_COL32_A_MASK;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    draw->PrimReserve(segments * 3, segments + 1);
    const ImDrawIdx base = ImDrawIdx(draw->_VtxCurrentIdx);
    draw->PrimWriteVtx(centre, uv, tint);
    for (int i = 0; i < segments; ++i) {
        const float a = float(i) / float(segments) * IM_PI * 2.0f;
        draw->PrimWriteVtx(ImVec2(centre.x + std::cos(a) * radius, centre.y + std::sin(a) * radius), uv, edge);
    }
    for (int i = 0; i < segments; ++i) {
        draw->PrimWriteIdx(base);
        draw->PrimWriteIdx(ImDrawIdx(base + 1 + i));
        draw->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % segments));
    }
}

// Layered translucent outlines read as a shadow without a blur pass.
inline void Shadow(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding, ImU32 tint, float spread = 18.0f, int layers = 7) {
    for (int i = layers; i >= 1; --i) {
        const float grow = spread * float(i) / float(layers);
        const float alpha = 1.0f / float(layers) * (1.0f - float(i - 1) / float(layers));
        draw->AddRectFilled(ImVec2(min.x - grow, min.y - grow * 0.6f), ImVec2(max.x + grow, max.y + grow * 1.2f),
                            color::Fade(tint, alpha), rounding + grow);
    }
}

// The window's ground: deep blue-black, a violet light upper left, a faint
// azure answer lower right. Drifts very slowly so the screen never looks dead.
inline void Backdrop(ImDrawList* draw, ImVec2 min, ImVec2 max) {
    draw->AddRectFilled(min, max, color::Background);
    const float t = float(ImGui::GetTime());
    const float w = max.x - min.x, h = max.y - min.y;
    Glow(draw, ImVec2(min.x + w * (0.12f + 0.02f * std::sin(t * 0.21f)), min.y + h * 0.02f), w * 0.62f,
         color::Fade(color::Violet, 0.16f), 96);
    Glow(draw, ImVec2(min.x + w * (0.92f + 0.02f * std::cos(t * 0.17f)), max.y + h * 0.05f), w * 0.55f,
         color::Fade(color::Azure, 0.09f), 96);
}

// ---------------------------------------------------------------- text
inline void Label(ImFont* font, float size, ImU32 tint, const char* text) {
    ImGui::PushFont(font, size);
    ImGui::PushStyleColor(ImGuiCol_Text, tint);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
inline void Wrapped(ImFont* font, float size, ImU32 tint, const char* text, float wrapAt = 0) {
    ImGui::PushFont(font, size);
    ImGui::PushStyleColor(ImGuiCol_Text, tint);
    ImGui::PushTextWrapPos(wrapAt > 0 ? ImGui::GetCursorPosX() + wrapAt : 0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
inline void Body(ImU32 tint, const char* text) { Wrapped(fonts().text, metric::TextBody, tint, text); }
inline void Small(ImU32 tint, const char* text) { Wrapped(fonts().text, metric::TextSmall, tint, text); }
inline void Display(const char* text, float size = metric::TextDisplay) { Label(fonts().display, size, color::Text, text); }
inline void Title(const char* text) { Label(fonts().strong, metric::TextTitle, color::Text, text); }
// Small caps-like section label: quiet, spaced, unmistakably a heading.
inline void Eyebrow(const char* text, ImU32 tint = color::Faint) { Label(fonts().strong, metric::TextTiny, tint, text); }
inline void Spacer(float height) { ImGui::Dummy(ImVec2(0, height)); }

inline ImVec2 IconSize(const char* glyph, float size) {
    ImGui::PushFont(fonts().icons, size);
    const ImVec2 measured = ImGui::CalcTextSize(glyph);
    ImGui::PopFont();
    return measured;
}
inline void IconAt(ImDrawList* draw, ImVec2 centre, const char* glyph, float size, ImU32 tint) {
    if (!fonts().icons) return;
    const ImVec2 measured = IconSize(glyph, size);
    draw->AddText(fonts().icons, size, ImVec2(std::floor(centre.x - measured.x * 0.5f), std::floor(centre.y - measured.y * 0.5f)),
                  tint, glyph);
}

// ---------------------------------------------------------------- surfaces
// A panel is a raised surface with a hairline edge and a thin line of light
// along its top. BeginPanel is always matched by EndPanel, whatever it
// returns: a clipped child still has to be closed.
inline bool BeginPanel(const char* id, ImVec2 size = ImVec2(0, 0), ImVec2 padding = ImVec2(24, 22), bool highlight = false) {
    // Light from the panel may spill past its edge, but only within the page it
    // sits on: clipping it at the panel would draw a visible rectangle.
    const ImRect page = ImGui::GetCurrentWindow()->ClipRect;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, metric::CardRadius);
    const ImGuiChildFlags flags = ImGuiChildFlags_AlwaysUseWindowPadding | (size.y == 0 ? ImGuiChildFlags_AutoResizeY : 0);
    const bool open = ImGui::BeginChild(id, size, flags, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max(min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight());
    draw->PushClipRect(page.Min, page.Max, false);
    GradientRect(draw, min, max, color::Surface, color::Mix(color::Surface, color::Background, 0.35f), metric::CardRadius);
    if (highlight) Glow(draw, ImVec2(min.x + (max.x - min.x) * 0.2f, min.y), (max.x - min.x) * 0.6f, color::Fade(color::Violet, 0.10f));
    draw->AddRect(min, max, color::Hairline, metric::CardRadius, 0, 1.0f);
    // light along the top edge, strongest in the middle, inside the hairline
    const float inset = metric::CardRadius * 1.6f, mid = (min.x + max.x) * 0.5f;
    GradientRect(draw, ImVec2(min.x + inset, min.y + 1), ImVec2(mid, min.y + 2), color::Fade(color::White, 0.0f), color::Fade(color::White, 0.07f));
    GradientRect(draw, ImVec2(mid, min.y + 1), ImVec2(max.x - inset, min.y + 2), color::Fade(color::White, 0.07f), color::Fade(color::White, 0.0f));
    draw->PopClipRect();
    return open;
}
inline void EndPanel() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

// ---------------------------------------------------------------- status
enum class Mark { Ok, Warn, Bad, Idle, Busy };

inline ImU32 MarkColor(Mark mark) {
    switch (mark) {
    case Mark::Ok: return color::Ok;
    case Mark::Warn: return color::Warn;
    case Mark::Bad: return color::Bad;
    case Mark::Busy: return color::Azure;
    default: return color::Faint;
    }
}
inline const char* MarkIcon(Mark mark) {
    switch (mark) {
    case Mark::Ok: return icon::Check;
    case Mark::Warn: return icon::Warning;
    case Mark::Bad: return icon::Error;
    case Mark::Busy: return icon::Sync;
    default: return icon::Info;
    }
}

// A spinning arc, for "working on it".
inline void SpinnerAt(ImDrawList* draw, ImVec2 centre, float radius, ImU32 tint, float thickness = 2.0f) {
    const float turn = float(ImGui::GetTime()) * 4.2f;
    draw->PathArcTo(centre, radius, turn, turn + IM_PI * 1.35f, 28);
    draw->PathStroke(tint, 0, thickness);
}

// The big state indicator at the top of a screen: a glowing ring with an icon.
// It breathes slowly when things are fine and turns while work is under way.
inline void StatusOrb(Mark mark, float radius) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 centre(origin.x + radius + 6, origin.y + radius + 6);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 tint = MarkColor(mark);
    const float t = float(ImGui::GetTime());
    const float breath = mark == Mark::Ok || mark == Mark::Idle ? 0.5f + 0.5f * std::sin(t * 1.6f) : 1.0f;
    draw->PushClipRect(ImVec2(centre.x - radius * 3, centre.y - radius * 3), ImVec2(centre.x + radius * 3, centre.y + radius * 3), false);
    Glow(draw, centre, radius * (2.1f + 0.25f * breath), color::Fade(tint, 0.20f + 0.10f * breath));
    draw->AddCircleFilled(centre, radius, color::Mix(color::Surface, tint, 0.14f), 64);
    draw->AddCircle(centre, radius, color::Fade(tint, 0.55f), 64, 1.5f);
    if (mark == Mark::Busy) SpinnerAt(draw, centre, radius + 5, tint, 2.2f);
    else draw->AddCircle(centre, radius + 5, color::Fade(tint, 0.14f + 0.12f * breath), 64, 1.0f);
    IconAt(draw, centre, MarkIcon(mark), radius * 0.78f, tint);
    draw->PopClipRect();
    ImGui::Dummy(ImVec2(radius * 2 + 12, radius * 2 + 12));
}

inline void Pill(Mark mark, const char* label) {
    ImGui::PushFont(fonts().strong, metric::TextTiny);
    const ImVec2 size = ImGui::CalcTextSize(label);
    ImGui::PopFont();
    const float height = size.y + 10, width = size.x + 30;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 tint = MarkColor(mark);
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), color::Fade(tint, 0.12f), height * 0.5f);
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height), color::Fade(tint, 0.28f), height * 0.5f);
    if (mark == Mark::Busy) SpinnerAt(draw, ImVec2(origin.x + 13, origin.y + height * 0.5f), 3.5f, tint, 1.6f);
    else draw->AddCircleFilled(ImVec2(origin.x + 13, origin.y + height * 0.5f), 3.2f, tint, 16);
    draw->AddText(fonts().strong, metric::TextTiny, ImVec2(origin.x + 22, origin.y + 5), tint, label);
    ImGui::Dummy(ImVec2(width, height));
}

// One fact about the machine: a tinted icon chip, what it is, what it says.
inline void CheckTile(const char* id, const char* glyph, const char* label, const char* value, Mark mark, float width) {
    const float height = 58;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::InvisibleButton("tile", ImVec2(width, height));
    const float hover = Approach(ImGui::GetID("hover"), ImGui::IsItemHovered() ? 1.0f : 0.0f);
    ImGui::PopID();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + width, origin.y + height);
    draw->AddRectFilled(origin, max, color::Mix(color::SurfaceHigh, color::Raised, hover * 0.5f), metric::ControlRadius);
    draw->AddRect(origin, max, color::Mix(color::Hairline, color::HairlineHi, hover), metric::ControlRadius);
    const ImU32 tint = MarkColor(mark);
    const ImVec2 chip(origin.x + 32, origin.y + height * 0.5f);
    draw->AddCircleFilled(chip, 17, color::Fade(tint, mark == Mark::Idle ? 0.10f : 0.14f), 32);
    if (mark == Mark::Busy) SpinnerAt(draw, chip, 9, tint, 2.0f);
    else IconAt(draw, chip, glyph, 15, mark == Mark::Idle ? color::Muted : tint);
    const float textX = origin.x + 60, right = max.x - 16;
    draw->AddText(fonts().strong, metric::TextSmall, ImVec2(textX, origin.y + 11), color::Text, label);
    ImGui::PushFont(fonts().text, metric::TextSmall);
    std::string shown = value;
    while (shown.size() > 3 && ImGui::CalcTextSize(shown.c_str()).x > right - textX) shown = shown.substr(0, shown.size() - 4) + u8"…";
    ImGui::PopFont();
    draw->AddText(fonts().text, metric::TextSmall, ImVec2(textX, origin.y + 30),
                  mark == Mark::Ok || mark == Mark::Idle ? color::Muted : tint, shown.c_str());
}

// ---------------------------------------------------------------- controls
// The one thing to do on a screen: an accent gradient that lifts and glows on
// hover and sinks when pressed. A hint line underneath says what will happen.
inline bool PrimaryButton(const char* label, const char* glyph, const char* hint, bool enabled, float width, float height = 0) {
    if (height <= 0) height = hint && *hint ? 64.0f : 48.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool held = hovered && ImGui::IsItemActive();
    const bool clicked = enabled && ImGui::IsItemDeactivated() && hovered;
    const ImGuiID id = ImGui::GetItemID();
    const float hover = Approach(id + 1, hovered ? 1.0f : 0.0f, 12.0f);
    const float press = Approach(id + 2, held ? 1.0f : 0.0f, 22.0f);
    const float alive = Approach(id + 3, enabled ? 1.0f : 0.0f, 10.0f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float lift = (hover * 1.5f - press * 1.0f);
    const ImVec2 min(origin.x, origin.y - lift), max(origin.x + width, origin.y + height - lift);
    draw->PushClipRect(ImVec2(min.x - 40, min.y - 30), ImVec2(max.x + 40, max.y + 40), false);
    if (alive > 0.01f) Shadow(draw, min, max, metric::ControlRadius, color::Fade(color::Violet, (0.18f + 0.22f * hover) * alive), 16, 8);
    const ImU32 from = color::Mix(color::Raised, color::Mix(color::Violet, color::White, hover * 0.10f - press * 0.0f), alive);
    const ImU32 to = color::Mix(color::Raised, color::Mix(color::Azure, color::White, hover * 0.10f), alive);
    GradientRect(draw, min, max, color::Mix(from, IM_COL32(0, 0, 0, 255), press * 0.12f),
                 color::Mix(to, IM_COL32(0, 0, 0, 255), press * 0.12f), metric::ControlRadius, 0, min, ImVec2(max.x, max.y + width * 0.15f));
    // glass: a brighter upper half and a crisp light edge
    draw->AddRectFilled(min, ImVec2(max.x, min.y + height * 0.5f), color::Fade(color::White, (0.07f + 0.03f * hover) * alive),
                        metric::ControlRadius, ImDrawFlags_RoundCornersTop);
    draw->AddRect(min, max, color::Fade(color::White, 0.10f + 0.12f * alive), metric::ControlRadius, 0, 1.0f);
    draw->PopClipRect();

    const ImU32 ink = color::Mix(color::Faint, color::White, alive);
    ImGui::PushFont(fonts().bold, metric::TextLead);
    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    ImGui::PopFont();
    const float glyphW = glyph ? IconSize(glyph, 17).x + 10 : 0;
    const bool withHint = hint && *hint;
    const float top = withHint ? min.y + 13 : min.y + (height - labelSize.y) * 0.5f;
    const float startX = min.x + (width - labelSize.x - glyphW) * 0.5f;
    if (glyph) IconAt(draw, ImVec2(startX + glyphW * 0.5f - 5, top + labelSize.y * 0.5f + 1), glyph, 17, ink);
    draw->AddText(fonts().bold, metric::TextLead, ImVec2(startX + glyphW, top), ink, label);
    if (withHint) {
        ImGui::PushFont(fonts().text, metric::TextSmall);
        const ImVec2 hintSize = ImGui::CalcTextSize(hint);
        ImGui::PopFont();
        draw->AddText(fonts().text, metric::TextSmall, ImVec2(min.x + (width - hintSize.x) * 0.5f, top + labelSize.y + 3),
                      color::Fade(ink, 0.78f), hint);
    }
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

// Everything else you can do: quiet until hovered.
inline bool SecondaryButton(const char* label, const char* glyph, bool enabled, float width = 0, float height = 40) {
    ImGui::PushFont(fonts().strong, metric::TextBody);
    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    ImGui::PopFont();
    const float glyphW = glyph ? IconSize(glyph, 14).x + 9 : 0;
    if (width <= 0) width = labelSize.x + glyphW + 36;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool held = hovered && ImGui::IsItemActive();
    const bool clicked = enabled && ImGui::IsItemDeactivated() && hovered;
    const float hover = Approach(ImGui::GetItemID() + 1, hovered ? 1.0f : 0.0f, 14.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + width, origin.y + height);
    const ImU32 fill = !enabled ? color::Fade(color::SurfaceHigh, 0.5f)
                                : color::Mix(color::Mix(color::SurfaceHigh, color::Raised, hover), color::Surface, held ? 0.4f : 0.0f);
    draw->AddRectFilled(origin, max, fill, metric::ControlRadius);
    draw->AddRect(origin, max, enabled ? color::Mix(color::HairlineHi, color::Fade(color::AccentText, 0.55f), hover) : color::Hairline,
                  metric::ControlRadius);
    const ImU32 ink = enabled ? color::Text : color::Faint;
    const float startX = origin.x + (width - labelSize.x - glyphW) * 0.5f;
    if (glyph) IconAt(draw, ImVec2(startX + (glyphW - 9) * 0.5f, origin.y + height * 0.5f + 1), glyph, 14,
                      enabled ? color::Mix(color::Muted, color::AccentText, hover) : color::Faint);
    draw->AddText(fonts().strong, metric::TextBody, ImVec2(startX + glyphW, origin.y + (height - labelSize.y) * 0.5f), ink, label);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

// A left-rail destination: icon and label, with a glowing marker that slides
// in when it becomes the current page.
inline bool NavItem(const char* glyph, const char* label, bool active, Mark badge, bool showBadge, float width) {
    const float height = 44;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemDeactivated() && hovered;
    const float on = Approach(ImGui::GetItemID() + 1, active ? 1.0f : 0.0f, 12.0f);
    const float hover = Approach(ImGui::GetItemID() + 2, hovered ? 1.0f : 0.0f, 16.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + width, origin.y + height);
    if (on > 0.01f || hover > 0.01f)
        draw->AddRectFilled(origin, max, color::Fade(color::Raised, 0.55f * hover + 0.85f * on), metric::ControlRadius);
    if (on > 0.01f) {
        const float bar = (height - 20) * on;
        GradientRect(draw, ImVec2(origin.x, origin.y + (height - bar) * 0.5f), ImVec2(origin.x + 3, origin.y + (height + bar) * 0.5f),
                     color::Violet, color::Azure, 2.0f, 0, ImVec2(origin.x, origin.y), ImVec2(origin.x, max.y));
        draw->PushClipRect(ImVec2(origin.x - 30, origin.y - 20), ImVec2(max.x, max.y + 20), false);
        Glow(draw, ImVec2(origin.x + 2, origin.y + height * 0.5f), 26, color::Fade(color::Violet, 0.22f * on));
        draw->PopClipRect();
    }
    const ImU32 ink = color::Mix(color::Mix(color::Muted, color::Text, hover), color::Text, on);
    IconAt(draw, ImVec2(origin.x + 26, origin.y + height * 0.5f + 1), glyph, 16, color::Mix(ink, color::AccentText, on));
    draw->AddText(fonts().strong, metric::TextBody, ImVec2(origin.x + 48, origin.y + (height - metric::TextBody * 1.33f) * 0.5f + 1), ink, label);
    if (showBadge) {
        const ImVec2 dot(max.x - 18, origin.y + height * 0.5f);
        if (badge == Mark::Busy) SpinnerAt(draw, dot, 5, MarkColor(badge), 1.8f);
        else draw->AddCircleFilled(dot, 4, MarkColor(badge), 16);
    }
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

// Progress that eases towards its value, with a sheen travelling along it.
inline void ProgressBar(const char* id, float fraction, float width, float height = 8) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float shown = Approach(ImGui::GetID(id), ImClamp(fraction, 0.0f, 1.0f), 8.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(origin.x + width, origin.y + height);
    draw->AddRectFilled(origin, max, color::Raised, height * 0.5f);
    const float fill = ImMax(height, width * shown);
    GradientRect(draw, origin, ImVec2(origin.x + fill, max.y), color::Violet, color::Azure, height * 0.5f, 0, origin, max);
    const float sweep = std::fmod(float(ImGui::GetTime()) * 0.7f, 1.4f) - 0.2f;
    const float sx = origin.x + fill * sweep;
    draw->PushClipRect(origin, ImVec2(origin.x + fill, max.y), true);
    GradientRect(draw, ImVec2(sx - 40, origin.y), ImVec2(sx, max.y), color::Fade(color::White, 0.0f), color::Fade(color::White, 0.35f));
    GradientRect(draw, ImVec2(sx, origin.y), ImVec2(sx + 40, max.y), color::Fade(color::White, 0.35f), color::Fade(color::White, 0.0f));
    draw->PopClipRect();
    ImGui::Dummy(ImVec2(width, height));
}

// A numbered step: the number chip beside its title, the sentence beneath.
inline void StepCard(const char* id, int number, const char* title, const char* text, float width, float height) {
    if (BeginPanel(id, ImVec2(width, height), ImVec2(18, 16))) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 centre(origin.x + 13, origin.y + 12);
        draw->AddCircleFilled(centre, 13, color::Fade(color::Violet, 0.18f), 32);
        draw->AddCircle(centre, 13, color::Fade(color::AccentText, 0.40f), 32);
        char digits[4];
        sprintf_s(digits, "%d", number);
        ImGui::PushFont(fonts().bold, metric::TextSmall);
        const ImVec2 size = ImGui::CalcTextSize(digits);
        ImGui::PopFont();
        draw->AddText(fonts().bold, metric::TextSmall, ImVec2(centre.x - size.x * 0.5f, centre.y - size.y * 0.5f), color::AccentText, digits);
        draw->AddText(fonts().strong, metric::TextBody, ImVec2(origin.x + 36, origin.y + 2), color::Text, title);
        ImGui::Dummy(ImVec2(26, 26));
        Spacer(2);
        Small(color::Muted, text);
    }
    EndPanel();
}

// A page arriving: settles upwards and a veil of the background dissolves off
// it over a quarter of a second. A veil, because custom drawing ignores
// ImGui's global alpha and half the page would otherwise pop in at once.
inline float PageEnter(int page) {
    static int current = -1;
    static double since = 0;
    if (page != current) { current = page; since = ImGui::GetTime(); }
    const float t = Ease(float((ImGui::GetTime() - since) / 0.30));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.0f - t) * 12.0f);
    return t;
}
inline void PageExit(float t) {
    if (t >= 1.0f) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
    const ImVec2 min = ImGui::GetWindowPos();
    draw->AddRectFilled(min, ImVec2(min.x + ImGui::GetWindowWidth(), min.y + ImGui::GetWindowHeight()),
                        color::Fade(color::Background, (1.0f - t) * 0.92f));
}

}  // namespace wardrobe::ui
