#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include "translations.h"

namespace wardrobe::language {
enum class Language { French, SimplifiedChinese };
inline std::atomic<Language> selected{Language::French};
inline Language Current() { return selected.load(); }
inline const char* Code(Language value) { return value == Language::SimplifiedChinese ? "zh-CN" : "fr"; }
inline Language Parse(std::string_view value) { return value == "zh-CN" ? Language::SimplifiedChinese : Language::French; }

// French source strings are catalog keys. Unknown text (including tool output)
// falls back unchanged. Translate only for display, never for job or widget IDs.
inline const char* Tr(const char* french) {
    if (!french || Current() == Language::French) return french;
    for (const auto& entry : Translations)
        if (std::string_view(french) == entry.french) return entry.chinese;
    return french;
}

inline std::filesystem::path SettingsPath() {
    wchar_t local[32768]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, DWORD(std::size(local)));
    if (!length || length >= std::size(local)) return {};
    return std::filesystem::path(local) / L"Wardrobe" / L"language.txt";
}

inline Language Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string code;
    // The setting is tiny; do not read an arbitrarily large or corrupt file.
    char bytes[8]{};
    in.read(bytes, sizeof(bytes));
    code.assign(bytes, size_t(in.gcount()));
    while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) code.pop_back();
    return Parse(code);
}

inline bool Save(const std::filesystem::path& path, Language value) {
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    auto staged = path;
    staged += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    {
        std::ofstream out(staged, std::ios::binary | std::ios::trunc);
        out << Code(value) << '\n';
        out.close();
        if (!out) { std::filesystem::remove(staged, error); return false; }
    }
    if (MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::filesystem::remove(staged, error);
    return false;
}

inline void Load() { selected = Read(SettingsPath()); }
inline bool Select(Language value) {
    if (!Save(SettingsPath(), value)) return false;
    selected = value;
    return true;
}

// Remove complete UTF-8 code points, so narrow status tiles never split a Hanzi.
template <typename Fits>
std::string Ellipsize(std::string text, Fits fits) {
    if (fits(text)) return text;
    while (!text.empty()) {
        size_t at = text.size() - 1;
        while (at && (static_cast<unsigned char>(text[at]) & 0xc0) == 0x80) --at;
        text.resize(at);
        const auto shortened = text + u8"…";
        if (fits(shortened)) return shortened;
    }
    return {};
}
} // namespace wardrobe::language
