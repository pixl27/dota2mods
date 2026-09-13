#pragma once
#include <string>
#include "imgui.h"
#include "imgui_internal.h"

// The visual system. One accent colour for things you can do, and warm colours
// reserved entirely for state, so nothing that merely looks clickable can be
// mistaken for a warning.
namespace wardrobe::ui {

namespace color {
constexpr ImU32 Background = IM_COL32(0x0E, 0x10, 0x14, 0xFF);
constexpr ImU32 Surface    = IM_COL32(0x1A, 0x1D, 0x24, 0xFF);
constexpr ImU32 Raised     = IM_COL32(0x26, 0x2B, 0x36, 0xFF);
constexpr ImU32 Border     = IM_COL32(0x30, 0x36, 0x45, 0xFF);
constexpr ImU32 Text       = IM_COL32(0xEC, 0xEF, 0xF5, 0xFF);
constexpr ImU32 Muted      = IM_COL32(0x99, 0xA1, 0xB3, 0xFF);
constexpr ImU32 Faint      = IM_COL32(0x66, 0x6E, 0x80, 0xFF);
constexpr ImU32 Accent     = IM_COL32(0x6E, 0x8A, 0xF0, 0xFF);
constexpr ImU32 AccentHigh = IM_COL32(0x8A, 0xA1, 0xFF, 0xFF);
constexpr ImU32 OnAccent   = IM_COL32(0x0E, 0x10, 0x16, 0xFF);
constexpr ImU32 Ok         = IM_COL32(0x4A, 0xDE, 0x80, 0xFF);
constexpr ImU32 Warn       = IM_COL32(0xFB, 0xBF, 0x24, 0xFF);
constexpr ImU32 Bad        = IM_COL32(0xF8, 0x71, 0x71, 0xFF);

inline ImVec4 Vec(ImU32 packed) { return ImGui::ColorConvertU32ToFloat4(packed); }
inline ImU32 Fade(ImU32 packed, float alpha) {
    ImVec4 value = Vec(packed);
    value.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(value);
}
}  // namespace color

namespace metric {
constexpr float CardRadius = 10.0f, ControlRadius = 8.0f;
constexpr float Gap = 8.0f, Pad = 16.0f, Section = 24.0f;
constexpr float TextSmall = 13.0f, TextBody = 15.0f, TextHeading = 20.0f, TextDisplay = 28.0f;
}  // namespace metric

struct Fonts {
    ImFont* text = nullptr;
    ImFont* strong = nullptr;
    ImFont* mono = nullptr;
};

inline Fonts& fonts() {
    static Fonts instance;
    return instance;
}

inline void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    // Latin-1 plus the arrows and geometric shapes used as status marks.
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x21FF, 0x2200, 0x22FF,
                                     0x2500, 0x257F, 0x25A0, 0x25FF, 0x2600, 0x27BF, 0};
    auto load = [&](const char* file, float size) -> ImFont* {
        char path[MAX_PATH]{};
        if (!GetWindowsDirectoryA(path, MAX_PATH)) return nullptr;
        strcat_s(path, "\\Fonts\\");
        strcat_s(path, file);
        return io.Fonts->AddFontFromFileTTF(path, size, nullptr, ranges);
    };
    Fonts& f = fonts();
    f.text = load("segoeui.ttf", metric::TextBody);
    f.strong = load("seguisb.ttf", metric::TextBody);
    f.mono = load("consola.ttf", metric::TextSmall);
    if (!f.text) f.text = io.Fonts->AddFontDefault();
    if (!f.strong) f.strong = f.text;
    if (!f.mono) f.mono = f.text;
    io.FontDefault = f.text;
}

inline void ApplyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(metric::Section, metric::Section);
    style.FramePadding = ImVec2(12, 8);
    style.ItemSpacing = ImVec2(metric::Gap, metric::Gap);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 10;
    style.WindowRounding = 0;
    style.ChildRounding = metric::CardRadius;
    style.FrameRounding = metric::ControlRadius;
    style.PopupRounding = metric::ControlRadius;
    style.ScrollbarRounding = 6;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.FrameBorderSize = 0;
    style.CellPadding = ImVec2(10, 8);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = color::Vec(color::Background);
    c[ImGuiCol_ChildBg] = color::Vec(color::Surface);
    c[ImGuiCol_PopupBg] = color::Vec(color::Raised);
    c[ImGuiCol_Border] = color::Vec(color::Border);
    c[ImGuiCol_Text] = color::Vec(color::Text);
    c[ImGuiCol_TextDisabled] = color::Vec(color::Faint);
    c[ImGuiCol_FrameBg] = color::Vec(color::Raised);
    c[ImGuiCol_FrameBgHovered] = color::Vec(color::Border);
    c[ImGuiCol_FrameBgActive] = color::Vec(color::Border);
    c[ImGuiCol_Button] = color::Vec(color::Raised);
    c[ImGuiCol_ButtonHovered] = color::Vec(color::Border);
    c[ImGuiCol_ButtonActive] = color::Vec(color::Fade(color::Border, 0.8f));
    c[ImGuiCol_Header] = color::Vec(color::Raised);
    c[ImGuiCol_HeaderHovered] = color::Vec(color::Border);
    c[ImGuiCol_HeaderActive] = color::Vec(color::Border);
    c[ImGuiCol_Separator] = color::Vec(color::Border);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = color::Vec(color::Border);
    c[ImGuiCol_ScrollbarGrabHovered] = color::Vec(color::Faint);
    c[ImGuiCol_ScrollbarGrabActive] = color::Vec(color::Muted);
}

// ---------------------------------------------------------------- primitives
inline void TextIn(ImU32 tint, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, tint);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

inline void WrappedIn(ImU32 tint, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, tint);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

inline void Sized(ImFont* font, float size, ImU32 tint, const char* text) {
    ImGui::PushFont(font, size);
    TextIn(tint, text);
    ImGui::PopFont();
}

inline void Heading(const char* text) {
    Sized(fonts().strong, metric::TextHeading, color::Text, text);
    ImGui::Dummy(ImVec2(0, 2));
}

inline void Small(ImU32 tint, const char* text) { Sized(fonts().text, metric::TextSmall, tint, text); }

inline void SmallWrapped(ImU32 tint, const char* text) {
    ImGui::PushFont(fonts().text, metric::TextSmall);
    WrappedIn(tint, text);
    ImGui::PopFont();
}

inline void Spacer(float height) { ImGui::Dummy(ImVec2(0, height)); }

// ---------------------------------------------------------------- containers
// A card is a plain surface with a hairline: enough to group, never enough to
// compete with the content inside it.
// BeginCard is always matched by EndCard, whatever it returns: a clipped child
// still has to be closed, and skipping that corrupts ImGui's window stack.
inline bool BeginCard(const char* id, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color::Vec(color::Surface));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, metric::CardRadius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18));
    const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    return open;
}

inline void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- status
enum class Mark { Ok, Warn, Bad, Idle, Busy };

inline ImU32 MarkColor(Mark mark) {
    switch (mark) {
    case Mark::Ok: return color::Ok;
    case Mark::Warn: return color::Warn;
    case Mark::Bad: return color::Bad;
    case Mark::Busy: return color::Accent;
    default: return color::Faint;
    }
}

// A filled dot for a settled state, a rotating arc while something is running.
inline void Dot(Mark mark, float radius = 4.5f) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetTextLineHeight();
    const ImVec2 centre(origin.x + radius + 1, origin.y + line * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (mark == Mark::Busy) {
        const float turn = float(ImGui::GetTime()) * 3.0f;
        draw->PathArcTo(centre, radius + 1.0f, turn, turn + 3.9f, 20);
        draw->PathStroke(MarkColor(mark), 0, 2.0f);
    } else {
        draw->AddCircleFilled(centre, radius, MarkColor(mark), 20);
        if (mark == Mark::Idle) draw->AddCircleFilled(centre, radius - 1.6f, color::Surface, 20);
    }
    ImGui::Dummy(ImVec2((radius + 1) * 2 + 4, line));
    ImGui::SameLine(0, 0);
}

inline void Pill(Mark mark, const char* label) {
    ImGui::PushFont(fonts().strong, metric::TextSmall);
    const ImVec2 size = ImGui::CalcTextSize(label);
    const float height = size.y + 8;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = size.x + 30;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 tint = MarkColor(mark);
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), color::Fade(tint, 0.14f), height * 0.5f);
    draw->AddCircleFilled(ImVec2(origin.x + 13, origin.y + height * 0.5f), 3.5f, tint, 16);
    draw->AddText(ImVec2(origin.x + 22, origin.y + 4), tint, label);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopFont();
}

// One line of the readiness list: mark, what it is, and what it says right now.
inline void CheckRow(Mark mark, const char* label, const char* value) {
    ImGui::Dummy(ImVec2(0, 3));
    Dot(mark);
    ImGui::PushFont(fonts().text, metric::TextBody);
    TextIn(color::Text, label);
    ImGui::PopFont();
    ImGui::SameLine();
    const float right = ImGui::GetContentRegionMax().x;
    ImGui::PushFont(fonts().text, metric::TextBody);
    const float width = ImGui::CalcTextSize(value).x;
    ImGui::SetCursorPosX(right - width);
    TextIn(mark == Mark::Ok ? color::Muted : MarkColor(mark), value);
    ImGui::PopFont();
}

inline void Step(int number, const char* text) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetTextLineHeight();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 centre(origin.x + 10, origin.y + line * 0.5f);
    draw->AddCircleFilled(centre, 10, color::Fade(color::Accent, 0.16f), 24);
    char label[4];
    sprintf_s(label, "%d", number);
    ImGui::PushFont(fonts().strong, metric::TextSmall);
    const ImVec2 size = ImGui::CalcTextSize(label);
    ImGui::PopFont();
    draw->AddText(fonts().strong, metric::TextSmall, ImVec2(centre.x - size.x * 0.5f, centre.y - size.y * 0.5f),
                  color::AccentHigh, label);
    ImGui::Dummy(ImVec2(28, line));
    ImGui::SameLine(0, 0);
    ImGui::PushFont(fonts().text, metric::TextBody);
    WrappedIn(color::Muted, text);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));
}

// ---------------------------------------------------------------- buttons
inline bool Action(const char* label, const char* hint, bool enabled, float width, bool primary = true) {
    const float height = hint && *hint ? 60.0f : 44.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool held = hovered && ImGui::IsItemActive();
    const bool clicked = enabled && ImGui::IsItemDeactivated() && hovered;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 corner(origin.x + width, origin.y + height);
    ImU32 fill;
    if (primary) fill = !enabled ? color::Raised : held ? color::Accent : hovered ? color::AccentHigh : color::Accent;
    else fill = !enabled ? color::Fade(color::Raised, 0.5f) : hovered ? color::Border : color::Raised;
    draw->AddRectFilled(origin, corner, fill, metric::ControlRadius);
    if (primary && enabled)
        // A brighter top edge gives the surface a light source instead of a flat slab.
        draw->AddRectFilled(origin, ImVec2(corner.x, origin.y + height * 0.5f), color::Fade(IM_COL32_WHITE, held ? 0.04f : 0.09f),
                            metric::ControlRadius, ImDrawFlags_RoundCornersTop);
    if (!primary) draw->AddRect(origin, corner, color::Border, metric::ControlRadius);

    const ImU32 labelTint = primary ? (enabled ? color::OnAccent : color::Faint)
                                    : (enabled ? color::Text : color::Faint);
    ImGui::PushFont(fonts().strong, metric::TextBody);
    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    ImGui::PopFont();
    const float textTop = hint && *hint ? origin.y + 12 : origin.y + (height - labelSize.y) * 0.5f;
    draw->AddText(fonts().strong, metric::TextBody, ImVec2(origin.x + (width - labelSize.x) * 0.5f, textTop), labelTint, label);
    if (hint && *hint) {
        ImGui::PushFont(fonts().text, metric::TextSmall);
        const ImVec2 hintSize = ImGui::CalcTextSize(hint);
        ImGui::PopFont();
        draw->AddText(fonts().text, metric::TextSmall, ImVec2(origin.x + (width - hintSize.x) * 0.5f, textTop + labelSize.y + 4),
                      color::Fade(labelTint, 0.72f), hint);
    }
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

inline bool Ghost(const char* label, bool enabled, float width = 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, color::Vec(enabled ? color::Text : color::Faint));
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, ImVec2(width, 0));
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    return clicked;
}

}  // namespace wardrobe::ui
