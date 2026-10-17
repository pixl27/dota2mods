#pragma once
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include "../../thirdparty/json.hpp"

namespace wardrobe::update {

// Date-based versions are numeric components, optionally prefixed by 'v'.
// Reject malformed/overflowing input before it can become a temporary filename.
inline bool VersionParts(const std::string& text, std::vector<uint32_t>& parts) {
    parts.clear();
    size_t at = !text.empty() && text.front() == 'v' ? 1 : 0;
    if (at == text.size()) return false;
    uint32_t value = 0;
    bool digit = false;
    for (; at < text.size(); ++at) {
        const char c = text[at];
        if (c >= '0' && c <= '9') {
            const auto next = uint32_t(c - '0');
            if (value > (std::numeric_limits<uint32_t>::max() - next) / 10) return false;
            value = value * 10 + next;
            digit = true;
        } else if (c == '.' && digit) {
            parts.push_back(value); value = 0; digit = false;
        } else return false;
    }
    if (!digit) return false;
    parts.push_back(value);
    return true;
}

inline bool Newer(const std::string& candidate, const std::string& current) {
    std::vector<uint32_t> a, b;
    if (!VersionParts(candidate, a) || !VersionParts(current, b)) return false;
    for (size_t i = 0; i < a.size() || i < b.size(); ++i) {
        const auto x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

enum class ReleaseState { Invalid, Current, Available, MissingInstaller };
struct Release {
    ReleaseState state = ReleaseState::Invalid;
    std::string version;
    std::string url;
};

inline Release ParseRelease(const std::string& body, const std::string& current) {
    Release result;
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (!json.is_object() || !json.contains("tag_name") || !json["tag_name"].is_string() ||
        !json.contains("assets") || !json["assets"].is_array()) return result;
    const auto tag = json["tag_name"].get<std::string>();
    std::vector<uint32_t> parts;
    if (!VersionParts(tag, parts) || !VersionParts(current, parts)) return result;
    result.version = tag.front() == 'v' ? tag.substr(1) : tag;
    if (!Newer(result.version, current)) {
        result.state = ReleaseState::Current;
        return result;
    }
    for (const auto& asset : json["assets"]) {
        if (!asset.is_object() || !asset.contains("name") || !asset["name"].is_string() ||
            !asset.contains("browser_download_url") || !asset["browser_download_url"].is_string()) continue;
        const auto name = asset["name"].get<std::string>();
        const auto url = asset["browser_download_url"].get<std::string>();
        if (name.rfind("Wardrobe-Setup-", 0) != 0 || name.size() <= 19 ||
            name.compare(name.size() - 4, 4, ".exe") != 0) continue;
        // Keep the existing loopback HTTP test endpoint, but reject other plaintext URLs.
        const bool https = url.rfind("https://", 0) == 0 && url.size() > 8;
        const bool loopback = url.rfind("http://127.0.0.1/", 0) == 0 || url.rfind("http://127.0.0.1:", 0) == 0;
        if (!https && !loopback) continue;
        result.state = ReleaseState::Available;
        result.url = url;
        return result;
    }
    result.state = ReleaseState::MissingInstaller;
    return result;
}
} // namespace wardrobe::update
