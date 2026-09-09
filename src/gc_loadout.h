#pragma once
#include "gc_protocol.h"
#include <map>
#include <set>
#include "appearance_state.h"

// Local cosmetic loadout. These messages are returned to Dota's receive queue;
// synthetic item IDs must never be submitted to Steam by the equip handler.
// Schemas: SteamDatabase/Protobufs/dota2/{econ,base,gcsdk}_gcmessages.proto.
namespace gc {
constexpr uint32_t AdjustEquipped = 1059, EquipItems = 2569, EquipItemsResponse = 2570;
constexpr uint32_t UpdateMultiple = 26;
using Slot = std::pair<uint32_t, uint32_t>; // hero/class, slot (zero is valid)
inline void Fixed64(Bytes& out, uint32_t field, uint64_t value) {
    Varint(out, (uint64_t(field) << 3) | 1);
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t(value >> (i * 8)));
}
inline uint64_t Value(const Bytes& bytes, uint32_t number, uint64_t fallback = 0) {
    std::vector<Field> fields;
    if (!Parse(bytes, fields)) return fallback;
    for (auto it = fields.rbegin(); it != fields.rend(); ++it)
        if (it->number == number && it->wire != 2) return it->value;
    return fallback;
}
inline Bytes RoutedPacket(uint32_t type, const Bytes& body, const Bytes& header) {
    Bytes out; Write32(out, type | ProtoFlag); Write32(out, uint32_t(header.size()));
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), body.begin(), body.end()); return out;
}
inline bool Unpack(uint32_t type, const void* data, size_t size, Bytes& body, Bytes& header) {
    if (!(type & ProtoFlag) || !data || size < 8 || size > MaxPacket) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    if (Read32(p) != type || Read32(p + 4) > size - 8) return false;
    const size_t at = 8 + Read32(p + 4);
    header.assign(p + 8, p + at); body.assign(p + at, p + size);
    std::vector<Field> fields;
    return Parse(header, fields) && Parse(body, fields);
}

enum class EquipState { PassThrough, Handled, Invalid };
struct EquipResult {
    EquipState state = EquipState::PassThrough;
    std::string detail;
    std::vector<PendingPacket> replies;
    uint32_t count = 0;
};
struct DiskInventory {
    bool valid = false;
    size_t items = 0, localItems = 0;
};
inline DiskInventory InspectDiskInventory(const Bytes& file, uint64_t steamId) {
    std::vector<Field> fields;
    if (!Parse(file, fields) || Value(file, 1) != 4) return {};
    DiskInventory result; result.valid = true;
    for (const auto& f : fields) if (f.number == 2 && f.wire == 2) {
        const auto cache = Payload(file, f); std::vector<Field> cacheFields;
        if (!Parse(cache, cacheFields)) return {};
        if (Value(cache, 1) != 1 || Value(cache, 2) != steamId) continue;
        for (const auto& cf : cacheFields) if (cf.number == 4 && cf.wire == 2) {
            const auto group = Payload(cache, cf); std::vector<Field> groupFields;
            if (!Parse(group, groupFields)) return {};
            if (Value(group, 1) != 1 || Value(group, 3) != 1) continue;
            for (const auto& gf : groupFields) if (gf.number == 2 && gf.wire == 2) {
                const auto item = Payload(group, gf); std::vector<Field> itemFields;
                if (!Parse(item, itemFields)) return {};
                ++result.items;
                if ((Value(item, 1) >> 62) == 1 && Value(item, 2) == uint32_t(steamId)) ++result.localItems;
            }
        }
    }
    return result;
}
class LocalLoadout {
    uint64_t steamId_ = 0, version_ = 0;
    uint32_t service_ = 0;
    bool econSnapshot_ = false;
    std::map<uint64_t, Bytes> catalog_, owned_;
    Bytes unequippedCatalog_;
    std::map<Slot, uint64_t> slots_;
    std::map<uint64_t, uint32_t> styles_;

    Bytes Decorate(uint64_t id, const Bytes& item, const std::map<Slot, uint64_t>& slots,
                   const std::map<uint64_t, uint32_t>& styles) const {
        Bytes out; std::vector<Field> fields; Parse(item, fields);
        for (const auto& f : fields) {
            if (f.number == 15 && styles.count(id)) continue;
            if (f.number == 18 && f.wire == 2) {
                const auto equipped = Payload(item, f);
                if (slots.count({uint32_t(Value(equipped, 1)), uint32_t(Value(equipped, 2))})) continue;
            }
            CopyField(out, item, f);
        }
        auto style = styles.find(id);
        if (style != styles.end()) Integer(out, 15, style->second);
        for (const auto& slot : slots) if (id && slot.second == id) {
            Bytes equipped; Integer(equipped, 1, slot.first.first); Integer(equipped, 2, slot.first.second);
            Blob(out, 18, equipped);
        }
        return out;
    }
    bool IsOwner(const Bytes& owner) const {
        return steamId_ && Value(owner, 1) == 1 && Value(owner, 2) == steamId_;
    }
    // Track real item changes too, so a later preview clears the item actually
    // equipped by Steam. Preserve the real attributes and decorate only field 18.
    Patch RewriteDelta(uint32_t type, const void* data, size_t size) {
        const auto kind = type & ~ProtoFlag;
        if (!(type & ProtoFlag) || (kind != 21 && kind != 22 && kind != 23 && kind != UpdateMultiple)) return {};
        Bytes body, header;
        if (!Unpack(type, data, size, body, header)) return {PatchState::Invalid, {}};
        std::vector<Field> fields; Parse(body, fields);
        const bool multiple = kind == UpdateMultiple;
        bool ours = false;
        for (const auto& f : fields) if (f.number == (multiple ? 6u : 5u) && f.wire == 2) ours = IsOwner(Payload(body, f));
        if (!ours) return {};
        const auto service = uint32_t(Value(body, multiple ? 7 : 6));
        if (econSnapshot_ && service != service_) return {};
        auto owned = owned_; bool econ = false, valid = true;
        auto changeItem = [&](const Bytes& item, bool removed) {
            std::vector<Field> itemFields;
            if (!Parse(item, itemFields)) { valid = false; return item; }
            const auto id = Value(item, 1);
            if (!id || IsLocal(id)) return item;
            econ = true;
            if (removed) { owned.erase(id); return item; }
            owned[id] = item;
            return Decorate(id, item, slots_, styles_);
        };
        Bytes changed;
        for (const auto& f : fields) {
            if (!multiple && Value(body, 2) == 1 && f.number == 3 && f.wire == 2) {
                Blob(changed, 3, changeItem(Payload(body, f), kind == 23));
            } else if (multiple && (f.number == 2 || f.number == 4 || f.number == 5) && f.wire == 2) {
                const auto object = Payload(body, f); std::vector<Field> objectFields;
                if (!Parse(object, objectFields)) return {PatchState::Invalid, {}};
                if (Value(object, 1) != 1) { CopyField(changed, body, f); continue; }
                Bytes rewritten;
                for (const auto& of : objectFields) {
                    if (of.number == 2 && of.wire == 2) Blob(rewritten, 2, changeItem(Payload(object, of), f.number == 5));
                    else CopyField(rewritten, object, of);
                }
                Blob(changed, f.number, rewritten);
            } else CopyField(changed, body, f);
        }
        if (!valid) return {PatchState::Invalid, {}};
        if (econ) {
            owned_ = std::move(owned); version_ = Value(body, multiple ? 3 : 4, version_); service_ = service;
        }
        if (changed == body) return {};
        return {PatchState::Modified, RoutedPacket(type, changed, header)};
    }
public:
    std::shared_ptr<const appearance::Snapshot> Appearance(uint64_t revision) const {
        auto snapshot = std::make_shared<appearance::Snapshot>();
        snapshot->revision = revision; snapshot->steamId = steamId_;
        for (const auto& slot : slots_) {
            const auto item = Item(slot.second);
            snapshot->selections.push_back({slot.first.first, slot.first.second, slot.second,
                uint32_t(Value(item, 4)), uint32_t(Value(item, 15))});
        }
        return snapshot;
    }
    bool RestoreDiskSelections(const Bytes& file) {
        if (!InspectDiskInventory(file, steamId_).valid) return false;
        std::map<uint32_t, uint64_t> ids;
        for (const auto& item : catalog_) ids[uint32_t(Value(item.second, 4))] = item.first;
        auto slots = slots_; auto styles = styles_;
        std::vector<Field> fields; Parse(file, fields);
        for (const auto& f : fields) if (f.number == 2 && f.wire == 2) {
            const auto cache = Payload(file, f); std::vector<Field> cf; Parse(cache, cf);
            if (Value(cache, 1) != 1 || Value(cache, 2) != steamId_) continue;
            for (const auto& c : cf) if (c.number == 4 && c.wire == 2) {
                const auto group = Payload(cache, c); std::vector<Field> gf; Parse(group, gf);
                if (Value(group, 1) != 1 || Value(group, 3) != 1) continue;
                for (const auto& g : gf) if (g.number == 2 && g.wire == 2) {
                    const auto item = Payload(group, g);
                    if ((Value(item, 1) >> 62) != 1 || Value(item, 2) != uint32_t(steamId_)) continue;
                    auto id = ids.find(uint32_t(Value(item, 4)));
                    if (id == ids.end()) continue;
                    std::vector<Field> itemFields; Parse(item, itemFields);
                    for (const auto& e : itemFields) if (e.number == 18 && e.wire == 2) {
                        const auto equipped = Payload(item, e);
                        const auto hero = Value(equipped, 1, UINT64_MAX), slot = Value(equipped, 2, UINT64_MAX);
                        if (hero > UINT32_MAX || slot >= 32) continue;
                        slots[{uint32_t(hero), uint32_t(slot)}] = id->second;
                        styles[id->second] = uint32_t(Value(item, 15));
                    }
                }
            }
        }
        slots_ = std::move(slots); styles_ = std::move(styles); return true;
    }
    void Reset(const std::vector<uint32_t>& definitions, uint64_t steamId, uint64_t firstId) {
        *this = {}; steamId_ = steamId;
        for (auto definition : definitions) {
            const auto id = firstId++;
            catalog_.emplace(id, EconItem(id, uint32_t(steamId), definition));
            Blob(unequippedCatalog_, 2, catalog_.at(id));
        }
    }
    bool IsLocal(uint64_t id) const { return catalog_.count(id) != 0; }
    Bytes Item(uint64_t id) const {
        auto local = catalog_.find(id);
        if (local != catalog_.end()) return Decorate(id, local->second, slots_, styles_);
        auto owned = owned_.find(id);
        return owned == owned_.end() ? Bytes{} : Decorate(id, owned->second, slots_, styles_);
    }
    Bytes CatalogFields() const {
        if (slots_.empty() && styles_.empty()) return unequippedCatalog_;
        Bytes out;
        for (const auto& item : catalog_) Blob(out, 2, Item(item.first));
        return out;
    }
    Patch RewriteCache(const Bytes& cache) {
        std::vector<Field> fields;
        if (!Parse(cache, fields)) return {PatchState::Invalid, {}};
        bool ours = false;
        for (const auto& f : fields) if (f.number == 4 && f.wire == 2) ours = IsOwner(Payload(cache, f));
        if (!ours) return {PatchState::NoLocalCache, {}};
        const auto cacheService = uint32_t(Value(cache, 5));
        const bool explicitService = Value(cache, 5, UINT64_MAX) != UINT64_MAX;
        // Dota's econ TypeCache belongs to service 1 (also in serialized .soc
        // files). Service 0 contains other account state; never inject an econ
        // group into it just because the owner matches. Legacy fixtures/builds
        // without service_id must contain an actual econ group to be eligible.
        if (explicitService && cacheService != 1) return {PatchState::NoLocalCache, {}};
        if (econSnapshot_ && cacheService != service_) return {PatchState::NoLocalCache, {}};
        Bytes out; auto owned = owned_;
        bool econGroup = false;
        uint64_t version = version_; uint32_t service = service_;
        for (const auto& f : fields) {
            if (f.number == 3 && f.wire == 1) version = f.value;
            if (f.number == 5 && f.wire == 0) service = uint32_t(f.value);
            if (f.number != 2 || f.wire != 2) { CopyField(out, cache, f); continue; }
            const auto group = Payload(cache, f); std::vector<Field> groupFields;
            if (!Parse(group, groupFields)) return {PatchState::Invalid, {}};
            if (Value(group, 1) != 1) { CopyField(out, cache, f); continue; }
            econGroup = true;
            owned.clear(); Bytes changed;
            for (const auto& gf : groupFields) {
                if (gf.number != 2 || gf.wire != 2) { CopyField(changed, group, gf); continue; }
                auto item = Payload(group, gf); std::vector<Field> itemFields;
                if (!Parse(item, itemFields)) return {PatchState::Invalid, {}};
                auto id = Value(item, 1);
                if (IsLocal(id) || ((id >> 62) == 1 && Value(item, 2) == uint32_t(steamId_)))
                    continue; // Discard saved preview IDs from earlier sessions too.
                if (id) owned[id] = item;
                Blob(changed, 2, Decorate(id, item, slots_, styles_));
            }
            Blob(out, 2, changed);
        }
        if (!econGroup && cacheService != 1) return {PatchState::NoLocalCache, {}};
        auto result = PatchCache(out, steamId_, CatalogFields());
        if (result.state == PatchState::Modified) {
            owned_ = std::move(owned); version_ = version; service_ = service;
            econSnapshot_ = econSnapshot_ || econGroup;
        }
        return result;
    }
    Patch RewritePacket(uint32_t type, const void* data, size_t size) {
        const auto kind = type & ~ProtoFlag;
        if (kind != CacheSubscribed && kind != ClientWelcome) return RewriteDelta(type, data, size);
        return PatchPacketWith(type, data, size, [&](const Bytes& cache) { return RewriteCache(cache); });
    }
    EquipResult Equip(uint32_t type, const void* data, size_t size) {
        const auto kind = type & ~ProtoFlag;
        if (kind != AdjustEquipped && kind != EquipItems) return {};
        Bytes body, header;
        if (!Unpack(type, data, size, body, header))
            return {EquipState::Invalid, "Equip packet format was not recognized"};
        std::vector<Bytes> requests; std::vector<Field> fields;
        Parse(body, fields);
        if (kind == AdjustEquipped) requests.push_back(body);
        else for (const auto& f : fields) if (f.number == 1 && f.wire == 2) requests.push_back(Payload(body, f));
        if (requests.empty() || requests.size() > 256)
            return {EquipState::Invalid, "Equip request is empty or too large"};
        struct Change { uint64_t id; uint32_t hero, slot, style; };
        std::vector<Change> changes; bool local = false;
        for (const auto& request : requests) {
            if (!Parse(request, fields)) return {EquipState::Invalid, "Equip entry is malformed"};
            // An omitted item_id means default/unequip; an omitted class or slot is ambiguous.
            const auto hero = Value(request, 2, UINT64_MAX), slot = Value(request, 3, UINT64_MAX);
            if (hero > UINT32_MAX || slot > UINT32_MAX)
                return {EquipState::Invalid, "Equip request is missing its hero or slot"};
            Change change{Value(request, 1), uint32_t(hero), uint32_t(slot), uint32_t(Value(request, 4, 255))};
            changes.push_back(change);
            local = local || IsLocal(change.id) || slots_.count({change.hero, change.slot});
        }
        if (!local) return {};
        auto slots = slots_; auto styles = styles_;
        for (const auto& change : changes) {
            if (change.id && !IsLocal(change.id) && !owned_.count(change.id))
                return {EquipState::Invalid, "Local outfit includes an unknown item; refresh inventory and retry"};
            if (change.slot == 65535 || change.slot == UINT32_MAX) {
                for (auto& entry : slots) if (entry.first.first == change.hero && entry.second == change.id) entry.second = 0;
            } else {
                // One instance cannot occupy two slots for the same hero.
                for (auto& entry : slots) if (change.id && entry.first.first == change.hero && entry.second == change.id) entry.second = 0;
                slots[{change.hero, change.slot}] = change.id;
            }
            if (change.id && change.style != 255) styles[change.id] = change.style;
        }
        Bytes update;
        auto addChanged = [&](uint64_t id, const Bytes& item) {
            auto before = Decorate(id, item, slots_, styles_);
            auto after = Decorate(id, item, slots, styles);
            if (before == after) return;
            Bytes object; Integer(object, 1, 1); Blob(object, 2, after); Blob(update, 2, object);
        };
        for (const auto& item : owned_) addChanged(item.first, item.second);
        // Only previously/newly selected catalog entries can change. Avoid
        // decoding the entire 13,000+ item catalog on every menu click.
        std::set<uint64_t> affected;
        for (const auto& entry : slots_) if (entry.second) affected.insert(entry.second);
        for (const auto& entry : slots) if (entry.second) affected.insert(entry.second);
        for (const auto& change : changes) if (change.id) affected.insert(change.id);
        for (auto id : affected) {
            auto found = catalog_.find(id);
            if (found != catalog_.end()) addChanged(id, found->second);
        }
        // Keep the GC's version; inventing a larger one would desynchronize later real updates.
        Fixed64(update, 3, version_);
        Bytes owner; Integer(owner, 1, 1); Integer(owner, 2, steamId_); Blob(update, 6, owner);
        if (service_) Integer(update, 7, service_);
        EquipResult result{EquipState::Handled, "Local equip update queued for Dota"};
        result.count = uint32_t(changes.size());
        result.replies.push_back({ProtoFlag | UpdateMultiple, Packet(UpdateMultiple, update)});
        if (kind == EquipItems) {
            Bytes replyHeader, reply;
            const auto job = Value(header, 10, UINT64_MAX);
            if (job != UINT64_MAX) Fixed64(replyHeader, 11, job);
            Integer(replyHeader, 13, 1); // EResult OK; complete the original client job.
            Fixed64(reply, 1, version_);
            result.replies.push_back({ProtoFlag | EquipItemsResponse, RoutedPacket(EquipItemsResponse, reply, replyHeader)});
        }
        for (const auto& reply : result.replies) if (reply.bytes.size() > MaxPacket)
            return {EquipState::Invalid, "Local equip update exceeds the receive buffer limit"};
        slots_ = std::move(slots); styles_ = std::move(styles);
        return result;
    }
};
}
