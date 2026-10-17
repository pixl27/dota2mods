#include "app/release.h"
#include <iostream>
#include <stdexcept>
using namespace wardrobe::update;
int main() {
    int checks = 0;
    auto check = [&](bool ok) { ++checks; if (!ok) throw std::runtime_error("release check " + std::to_string(checks)); };
    try {
        for (const auto* body : {"", "{}", "[]", "null", "<html>error</html>",
             "{\"tag_name\":\"v2026.10.08.1\",", "{\"tag_name\":null,\"assets\":[]}",
             "{\"tag_name\":123,\"assets\":[]}", "{\"tag_name\":\"v2026.10.08.1\"}",
             "{\"tag_name\":\"v2026.10.08.1\",\"assets\":{}}"})
            check(ParseRelease(body, "2026.10.07.2").state == ReleaseState::Invalid);
        for (const auto* tag : {"", "v", "latest", "1..2", "1.", "../2026.10.08", "1.4294967296", "1.0-rc1"}) {
            const auto body = nlohmann::json{{"tag_name",tag},{"assets",nlohmann::json::array()}}.dump();
            check(ParseRelease(body, "2026.10.07.2").state == ReleaseState::Invalid);
        }
        check(Newer("v2026.10.07.10", "2026.10.07.2"));
        check(!Newer("2026.10.07.2.0", "2026.10.07.2"));
        check(!Newer("invalid", "2026.10.07.2"));
        check(ParseRelease(R"({"tag_name":"v2026.10.07.2","assets":[]})", "2026.10.07.2").state == ReleaseState::Current);
        check(ParseRelease(R"({"tag_name":"v2026.10.07.1","assets":[]})", "2026.10.07.2").state == ReleaseState::Current);
        auto release = nlohmann::json{{"tag_name","v2026.10.08.1"},{"assets",nlohmann::json::array()}};
        check(ParseRelease(release.dump(), "2026.10.07.2").state == ReleaseState::MissingInstaller);
        release["body"] = "browser_download_url: https://example.com/Wardrobe-Setup-fake.exe";
        release["assets"] = {nullptr, 42, {{"name","notes.txt"},{"browser_download_url","https://example.com/Wardrobe-Setup-fake.exe"}},
            {{"name","Wardrobe-Setup-2026.10.08.1.exe"},{"browser_download_url",17}}};
        check(ParseRelease(release.dump(), "2026.10.07.2").state == ReleaseState::MissingInstaller);
        release["assets"].push_back({{"name","Wardrobe-Setup-2026.10.08.1.exe"},{"browser_download_url","http://example.com/setup.exe"}});
        check(ParseRelease(release.dump(), "2026.10.07.2").state == ReleaseState::MissingInstaller);
        const std::string url = "https://example.com/Wardrobe-Setup-2026.10.08.1.exe";
        release["assets"].push_back({{"name","Wardrobe-Setup-2026.10.08.1.exe"},{"browser_download_url",url}});
        auto result = ParseRelease(release.dump(), "2026.10.07.2");
        check(result.state == ReleaseState::Available && result.version == "2026.10.08.1" && result.url == url);
        std::cout << "PASS: " << checks << " release response checks\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
