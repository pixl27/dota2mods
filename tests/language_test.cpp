#include "app/language.h"
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>
using namespace wardrobe::language;
// Compare complete printf conversions, including length modifiers (e.g. %zu).
std::vector<std::string> Formats(const char* text) {
    std::vector<std::string> out;
    const std::string s(text);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') continue;
        if (i + 1 < s.size() && s[i+1] == '%') { ++i; continue; }
        const auto start = i++;
        while (i < s.size() && std::string("diuoxXfFeEgGaAcspn").find(s[i]) == std::string::npos) ++i;
        if (i < s.size()) out.push_back(s.substr(start, i-start+1));
    }
    return out;
}
int main() {
    int checks=0;
    auto check=[&](bool ok) { ++checks; if(!ok) throw std::runtime_error("language check " + std::to_string(checks)); };
    auto root=std::filesystem::temp_directory_path() / (L"Wardrobe-language-test-中文-" + std::to_wstring(GetCurrentProcessId()));
    try {
        check(Parse("zh-CN") == Language::SimplifiedChinese);
        check(Parse("unknown") == Language::French);
        check(Read(root / L"missing.txt") == Language::French);
        std::set<std::string> keys;
        for (const auto& entry : Translations) {
            check(keys.insert(entry.french).second);
            check(*entry.chinese != '\0');
            check(Formats(entry.french) == Formats(entry.chinese));
            selected=Language::French;
            check(std::string(Tr(entry.french)) == entry.french);
            selected=Language::SimplifiedChinese;
            check(std::string(Tr(entry.french)) == entry.chinese);
        }
        check(std::string(Tr("--accept-changes")) == "--accept-changes");
        check(Tr(nullptr) == nullptr);
        check(std::string(Tr("Accueil")) == "首页");
        check(std::string(Tr("Régénérer le profil de Dota")) == "重新生成 Dota 2 适配配置");
        const auto path=root/L"嵌套"/L"language.txt";
        check(Save(path, Language::SimplifiedChinese));
        check(Read(path) == Language::SimplifiedChinese);
        check(Save(path, Language::French));
        check(Read(path) == Language::French);
        {std::ofstream out(path); out << "zh-CN\r\n";}
        check(Read(path) == Language::SimplifiedChinese);
        {std::ofstream out(path); out << "bad language setting";}
        check(Read(path) == Language::French);
        check(!Save({},Language::French));
        check(!Save(path/L"child",Language::French)); // parent is a file
        check(Ellipsize("中文状态", [](const auto& s) {return s.size() <= 9;}) == "中文…");
        check(Ellipsize("Français", [](const auto& s) {return s.size() <= 9;}) == "Français");
        check(Ellipsize("中文", [](const auto&) {return false;}).empty());
        std::filesystem::remove_all(root);
        std::cout << "PASS: " << checks << " language/catalog/persistence checks\n";
    } catch(const std::exception& e) {
        std::error_code ignored; std::filesystem::remove_all(root,ignored);
        std::cerr << e.what() << '\n'; return 1;
    }
}
