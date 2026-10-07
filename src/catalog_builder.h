#pragma once
// The cosmetic catalog (data/skins_full.json), built without Python.
//
// A port of gen_full_db.py followed by gen_names.py, held to the same output:
// tests/test_catalog_builder.py requires the JSON to be identical to what the
// two scripts write from the same game files. That is what lets a copy of
// Wardrobe on a machine without Python pick up the cosmetics of a Dota content
// update by itself.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace wardrobe::catalog_builder {

// ---------------------------------------------------------------- KeyValues
// Valve's text format as the Python `vdf` package reads it: quoted or bare
// tokens, nested blocks, // comments, [$CONDITION] suffixes ignored, duplicate
// keys kept in order (lookups return the first).
struct Node {
    struct Item {
        std::string key, text;
        std::unique_ptr<Node> node;
    };
    std::vector<Item> items;
    const Item* Find(const std::string& key) const {
        for (const auto& item : items) if (item.key == key) return &item;
        return nullptr;
    }
    const Node* Child(const std::string& key) const {
        const Item* item = Find(key);
        return item && item->node ? item->node.get() : nullptr;
    }
    // A string value, or nullptr when absent or a block (Python's scalar()).
    const std::string* Text(const std::string& key) const {
        const Item* item = Find(key);
        return item && !item->node ? &item->text : nullptr;
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}
    std::unique_ptr<Node> Parse() {
        auto root = std::make_unique<Node>();
        Block(*root, false);
        return root;
    }

private:
    enum class Kind { End, Text, Open, Close };
    void Skip() {
        while (at_ < s_.size()) {
            const char c = s_[at_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++at_;
            else if (c == '/' && at_ + 1 < s_.size() && s_[at_ + 1] == '/') { while (at_ < s_.size() && s_[at_] != '\n') ++at_; }
            else if (c == '[') { while (at_ < s_.size() && s_[at_] != ']' && s_[at_] != '\n') ++at_; if (at_ < s_.size()) ++at_; }
            else break;
        }
    }
    Kind Next(std::string& out) {
        Skip();
        if (at_ >= s_.size()) return Kind::End;
        const char c = s_[at_];
        if (c == '{') { ++at_; return Kind::Open; }
        if (c == '}') { ++at_; return Kind::Close; }
        out.clear();
        if (c == '"') {
            ++at_;
            while (at_ < s_.size() && s_[at_] != '"') {
                if (s_[at_] == '\\' && at_ + 1 < s_.size()) {
                    const char e = s_[at_ + 1];
                    const char* from = "ntvbrfa\\?'\"";
                    const char* to = "\n\t\v\b\r\f\a\\?'\"";
                    if (const char* hit = strchr(from, e); hit && e) { out.push_back(to[hit - from]); at_ += 2; continue; }
                }
                out.push_back(s_[at_++]);
            }
            ++at_;
            return Kind::Text;
        }
        while (at_ < s_.size() && !strchr(" \t\r\n{}\"", s_[at_])) out.push_back(s_[at_++]);
        return Kind::Text;
    }
    void Block(Node& node, bool nested) {
        std::string key, value;
        for (;;) {
            Kind kind = Next(key);
            if (kind == Kind::End) return;
            if (kind == Kind::Close) { if (nested) return; continue; }
            if (kind == Kind::Open) {   // a block without a key: keep its content reachable under ""
                Node::Item item{"", "", std::make_unique<Node>()};
                Block(*item.node, true);
                node.items.push_back(std::move(item));
                continue;
            }
            kind = Next(value);
            if (kind == Kind::Open) {
                Node::Item item{key, "", std::make_unique<Node>()};
                Block(*item.node, true);
                node.items.push_back(std::move(item));
            } else if (kind == Kind::Text) {
                node.items.push_back(Node::Item{key, value, nullptr});
            } else {
                if (kind == Kind::Close && nested) return;
                if (kind == Kind::End) return;
            }
        }
    }
    const std::string& s_;
    size_t at_ = 0;
};

// ---------------------------------------------------------------- helpers
inline std::string Lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return text;
}
inline bool Digits(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}
inline std::string FirstWord(const std::string& text) {
    const size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return {};
    const size_t end = text.find_first_of(" \t", start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// gen_full_db.Prefabs.resolve, including its quirk: when the first name of a
// multi-prefab chain has a parent, the search moves to that parent and the
// other names of the chain are not visited.
inline const std::string* Resolve(const Node& prefabs, const Node& item, const std::string& key) {
    if (const std::string* own = item.Text(key); own && !own->empty()) return own;
    std::set<std::string> seen;
    const std::string* start = item.Text("prefab");
    std::string chain = start ? *start : std::string();
    while (!chain.empty()) {
        bool moved = false;
        size_t at = 0;
        while (at < chain.size()) {
            while (at < chain.size() && (chain[at] == ' ' || chain[at] == '\t')) ++at;
            size_t end = at;
            while (end < chain.size() && chain[end] != ' ' && chain[end] != '\t') ++end;
            if (end == at) break;
            const std::string name = chain.substr(at, end - at);
            at = end;
            if (seen.count(name)) continue;
            seen.insert(name);
            const Node* body = prefabs.Child(name);
            if (!body) continue;
            if (const std::string* value = body->Text(key); value && !value->empty()) return value;
            const std::string* parent = body->Text("prefab");
            if (parent && !parent->empty() && !seen.count(*parent)) { chain = *parent; moved = true; break; }
        }
        if (!moved) chain.clear();
    }
    return nullptr;
}

// ---------------------------------------------------------------- the catalog
struct Bundle { int64_t id; std::string name; };
struct Skin {
    int64_t def = 0;
    std::string name, slot, rarity, prefab, type, hero;
    bool hasType = false;
    size_t styles = 0;
    int64_t persona = 0;
    std::vector<Bundle> bundles;
};
struct Catalog {
    std::vector<std::string> heroes;
    std::vector<Skin> skins;
};

inline const std::set<std::string>& Excluded() {
    static const std::set<std::string> set = {"bundle", "treasure_chest", "retired_treasure_chest", "key", "socket_gem", "league", "tool",
                                              "sticker", "sticker_capsule", "dynamic_recipe", "player_card", "emoticon_tool",
                                              "showcase_decoration"};
    return set;
}
inline const std::set<std::string>& ClientGlobals() {
    static const std::set<std::string> set = {"loading_screen", "hud_skin", "music", "terrain", "emblem", "cursor_pack", "announcer", "versus_screen"};
    return set;
}

inline Catalog Build(const Node& root) {
    const Node* game = root.Child("items_game");
    if (!game) game = &root;
    static const Node empty;
    const Node& prefabs = game->Child("prefabs") ? *game->Child("prefabs") : empty;
    const Node& items = game->Child("items") ? *game->Child("items") : empty;
    Catalog catalog;
    std::set<std::string> heroes;
    std::unordered_map<std::string, int64_t> nameToDef;
    for (const auto& entry : items.items) {
        if (!Digits(entry.key) || !entry.node) continue;
        const Node& body = *entry.node;
        const int64_t def = std::stoll(entry.key);
        const std::string* internal = body.Text("name");
        if (internal && !internal->empty()) nameToDef[Lower(*internal)] = def;
        const std::string* prefab = body.Text("prefab");
        const std::string prefabKey = prefab && !prefab->empty() ? FirstWord(*prefab) : std::string();
        if (Excluded().count(prefabKey)) continue;
        std::vector<std::string> targets;
        if (const Node::Item* used = body.Find("used_by_heroes")) {
            if (used->node) {
                for (const auto& hero : used->node->items)
                    if (!hero.node && hero.text == "1" && Lower(hero.key).rfind("npc_dota_hero_", 0) == 0) targets.push_back(Lower(hero.key));
            } else if (Lower(used->text).rfind("npc_dota_hero_", 0) == 0) {
                targets.push_back(Lower(used->text));
            }
        }
        for (const auto& hero : targets)
            if (!(hero.size() >= 5 && hero.compare(hero.size() - 5, 5, "_base") == 0) && hero.find("target_dummy") == std::string::npos)
                heroes.insert(hero);
        if (targets.empty()) {
            if (ClientGlobals().count(prefabKey)) targets.push_back("_global");
            else continue;
        }
        Skin skin;
        skin.def = def;
        if (const std::string* name = body.Text("item_name"); name && !name->empty()) skin.name = *name;
        else if (const std::string* inherited = Resolve(prefabs, body, "item_name")) skin.name = *inherited;
        else if (internal) skin.name = *internal;
        if (const std::string* slot = Resolve(prefabs, body, "item_slot")) skin.slot = *slot;
        else if (const std::string* equip = body.Text("equipment_slot"); equip && !equip->empty()) skin.slot = *equip;
        else skin.slot = targets.size() == 1 && targets[0] == "_global" ? "global" : "misc";
        const std::string* rarity = Resolve(prefabs, body, "item_rarity");
        skin.rarity = rarity ? *rarity : "common";
        skin.prefab = prefabKey;
        if (const std::string* type = Resolve(prefabs, body, "item_type_name")) { skin.type = *type; skin.hasType = true; }
        if (const Node* visuals = body.Child("visuals")) {
            if (const Node* styles = visuals->Child("styles")) skin.styles = styles->items.size();
            for (const auto& modifier : visuals->items) {
                if (modifier.key != "asset_modifier" || !modifier.node) continue;
                const std::string* kind = modifier.node->Text("type");
                if (!kind || *kind != "persona") continue;
                const std::string* persona = modifier.node->Text("persona");
                const std::string value = persona && !persona->empty() ? *persona : "0";
                skin.persona = Digits(value) ? std::stoll(value) : 0;
                break;
            }
        }
        for (const auto& hero : targets) {
            Skin copy = skin;
            copy.hero = hero;
            catalog.skins.push_back(std::move(copy));
        }
    }
    // Sets: each piece lists every set it belongs to, numbered in file order.
    std::unordered_map<int64_t, std::vector<Bundle>> bundles;
    int64_t setIndex = 100000;
    if (const Node* sets = game->Child("item_sets")) {
        for (const auto& set : sets->items) {
            if (!set.node) continue;
            const std::string* setName = set.node->Text("name");
            const std::string name = setName && !setName->empty() ? *setName : set.key;
            if (const Node* members = set.node->Child("items")) {
                for (const auto& member : members->items) {
                    int64_t target = 0;
                    if (Digits(member.key)) target = std::stoll(member.key);
                    else if (auto hit = nameToDef.find(Lower(member.key)); hit != nameToDef.end()) target = hit->second;
                    if (target) bundles[target].push_back({setIndex, name});
                }
            }
            ++setIndex;
        }
    }
    for (auto& skin : catalog.skins)
        if (auto hit = bundles.find(skin.def); hit != bundles.end()) skin.bundles = hit->second;
    catalog.heroes.assign(heroes.begin(), heroes.end());
    return catalog;
}

// ---------------------------------------------------------------- names
// gen_names.py: "key" "value" pairs from the localization files, first file
// wins, keys compared lowercase without their '#'.
inline void LoadTokens(const std::string& text, std::unordered_map<std::string, std::string>& tokens) {
    size_t i = 0;
    const size_t n = text.size();
    while ((i = text.find('"', i)) != std::string::npos) {
        size_t k = i + 1;
        while (k < n && text[k] != '"' && text[k] != '\n') ++k;
        if (k == i + 1 || k >= n || text[k] != '"') { ++i; continue; }
        size_t w = k + 1;
        while (w < n && (text[w] == ' ' || text[w] == '\t' || text[w] == '\n' || text[w] == '\r' || text[w] == '\f' || text[w] == '\v')) ++w;
        if (w == k + 1 || w >= n || text[w] != '"') { ++i; continue; }
        size_t v = w + 1;
        while (v < n && text[v] != '"' && text[v] != '\n') ++v;
        if (v >= n || text[v] != '"') { ++i; continue; }
        std::string key = Lower(text.substr(i + 1, k - i - 1));
        key.erase(0, key.find_first_not_of('#'));
        tokens.emplace(std::move(key), text.substr(w + 1, v - w - 1));
        i = v + 1;
    }
}

inline std::string Derive(const std::string& token) {
    std::string text = token;
    text.erase(0, text.find_first_not_of('#'));
    for (const char* prefix : {"DOTA_Item_", "DOTA_Wearable_", "DOTA_", "npc_dota_"})
        if (text.rfind(prefix, 0) == 0) { text = text.substr(strlen(prefix)); break; }
    for (auto& c : text) if (c == '_') c = ' ';
    const size_t first = text.find_first_not_of(" \t\r\n"), last = text.find_last_not_of(" \t\r\n");
    text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    return text.empty() ? token : text;
}

struct NameReport { size_t total = 0, resolved = 0, derived = 0; };

inline NameReport ApplyNames(Catalog& catalog, const std::unordered_map<std::string, std::string>& tokens) {
    NameReport report;
    auto resolve = [&](std::string& text) {
        ++report.total;
        if (text.empty() || text[0] != '#') { ++report.resolved; return; }
        std::string key = Lower(text);
        key.erase(0, key.find_first_not_of('#'));
        if (auto hit = tokens.find(key); hit != tokens.end()) { text = hit->second; ++report.resolved; return; }
        text = Derive(text);
        ++report.derived;
    };
    for (auto& skin : catalog.skins) {
        resolve(skin.name);
        if (skin.hasType) resolve(skin.type);
        for (auto& bundle : skin.bundles) resolve(bundle.name);
    }
    return report;
}

// ---------------------------------------------------------------- JSON
// json.dump(indent=1) with Python's defaults: ASCII only, \uXXXX escapes.
inline void JsonString(std::string& out, const std::string& text) {
    out.push_back('"');
    size_t i = 0;
    while (i < text.size()) {
        const uint8_t c = uint8_t(text[i]);
        uint32_t code = 0;
        size_t length = 0;
        if (c < 0x80) { code = c; length = 1; }
        else if ((c & 0xE0) == 0xC0) { code = c & 0x1F; length = 2; }
        else if ((c & 0xF0) == 0xE0) { code = c & 0x0F; length = 3; }
        else if ((c & 0xF8) == 0xF0) { code = c & 0x07; length = 4; }
        else { ++i; continue; }   // invalid lead byte: dropped, like errors="ignore"
        bool valid = i + length <= text.size();
        for (size_t k = 1; valid && k < length; ++k) {
            const uint8_t b = uint8_t(text[i + k]);
            if ((b & 0xC0) != 0x80) valid = false;
            else code = (code << 6) | (b & 0x3F);
        }
        if (!valid) { ++i; continue; }
        i += length;
        char buffer[16];
        switch (code) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (code < 0x20 || code >= 0x7F) {
                if (code >= 0x10000) {
                    const uint32_t v = code - 0x10000;
                    sprintf_s(buffer, "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
                } else {
                    sprintf_s(buffer, "\\u%04x", code);
                }
                out += buffer;
            } else {
                out.push_back(char(code));
            }
        }
    }
    out.push_back('"');
}

inline std::string ToJson(const Catalog& catalog) {
    std::string out = "{\n \"heroes\": [";
    for (size_t i = 0; i < catalog.heroes.size(); ++i) {
        out += i ? ",\n  " : "\n  ";
        JsonString(out, catalog.heroes[i]);
    }
    out += catalog.heroes.empty() ? "]" : "\n ]";
    out += ",\n \"skins\": [";
    for (size_t i = 0; i < catalog.skins.size(); ++i) {
        const Skin& s = catalog.skins[i];
        out += i ? ",\n  {" : "\n  {";
        out += "\n   \"def\": " + std::to_string(s.def);
        out += ",\n   \"name\": "; JsonString(out, s.name);
        out += ",\n   \"slot\": "; JsonString(out, s.slot);
        out += ",\n   \"rarity\": "; JsonString(out, s.rarity);
        out += ",\n   \"prefab\": "; JsonString(out, s.prefab);
        if (s.hasType) { out += ",\n   \"type\": "; JsonString(out, s.type); }
        if (s.styles > 1) out += ",\n   \"styles\": " + std::to_string(s.styles);
        if (s.persona) out += ",\n   \"persona\": " + std::to_string(s.persona);
        out += ",\n   \"hero\": "; JsonString(out, s.hero);
        if (!s.bundles.empty()) {
            out += ",\n   \"bundles\": [";
            for (size_t b = 0; b < s.bundles.size(); ++b) {
                out += b ? ",\n    {" : "\n    {";
                out += "\n     \"id\": " + std::to_string(s.bundles[b].id);
                out += ",\n     \"name\": "; JsonString(out, s.bundles[b].name);
                out += "\n    }";
            }
            out += "\n   ]";
        }
        out += "\n  }";
    }
    out += catalog.skins.empty() ? "]" : "\n ]";
    out += "\n}";
    return out;
}

}  // namespace wardrobe::catalog_builder
