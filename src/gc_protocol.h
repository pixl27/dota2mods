#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// Wire schema: SteamTracking/Protobufs/dota2/{gcsdk,base}_gcmessages.proto.
// This code is independent of Windows and never sends modified items to Steam.
namespace gc {
using Bytes = std::vector<uint8_t>;
constexpr uint32_t ProtoFlag = 0x80000000u;
constexpr uint32_t CacheSubscribed = 24, CacheRefresh = 28, ClientWelcome = 4004;
constexpr size_t MaxPacket = 64 * 1024 * 1024;
enum Result { OK = 0, NoMessage = 1, BufferTooSmall = 2, NotLoggedOn = 3, InvalidMessage = 4 };

inline void Varint(Bytes& out, uint64_t value) {
    while (value > 127) { out.push_back(uint8_t(value) | 0x80); value >>= 7; }
    out.push_back(uint8_t(value));
}
inline void Integer(Bytes& out, uint32_t field, uint64_t value) {
    Varint(out, uint64_t(field) << 3); Varint(out, value);
}
inline void Blob(Bytes& out, uint32_t field, const Bytes& data) {
    Varint(out, (uint64_t(field) << 3) | 2); Varint(out, data.size());
    out.insert(out.end(), data.begin(), data.end());
}
inline uint32_t Read32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void Write32(Bytes& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (i * 8)));
}
inline bool ReadVarint(const Bytes& in, size_t& at, uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (at == in.size()) return false;
        uint8_t byte = in[at++];
        if (shift == 63 && byte > 1) return false;
        value |= uint64_t(byte & 127) << shift;
        if (!(byte & 128)) return true;
    }
    return false;
}
struct Field {
    uint32_t number = 0, wire = 0;
    size_t begin = 0, payload = 0, end = 0;
    uint64_t value = 0;
};
inline bool Parse(const Bytes& in, std::vector<Field>& fields) {
    fields.clear();
    if (in.size() > MaxPacket) return false;
    size_t at = 0;
    while (at < in.size()) {
        Field f; f.begin = at;
        uint64_t tag = 0, length = 0;
        if (!ReadVarint(in, at, tag) || !(tag >> 3) || (tag >> 3) > 0x1fffffffu) return false;
        f.number = uint32_t(tag >> 3); f.wire = uint32_t(tag & 7); f.payload = at;
        switch (f.wire) {
        case 0: if (!ReadVarint(in, at, f.value)) return false; break;
        case 1: case 5:
            length = f.wire == 1 ? 8 : 4;
            if (length > in.size() - at) return false;
            for (size_t i = 0; i < length; ++i) f.value |= uint64_t(in[at++]) << (i * 8);
            break;
        case 2:
            if (!ReadVarint(in, at, length) || length > in.size() - at) return false;
            f.payload = at; at += size_t(length); break;
        default: return false; // Unrecognized/group fields are passed through without editing.
        }
        f.end = at; fields.push_back(f);
    }
    return true;
}
inline Bytes Payload(const Bytes& in, const Field& f) {
    return Bytes(in.begin() + f.payload, in.begin() + f.end);
}
inline void CopyField(Bytes& out, const Bytes& in, const Field& f) {
    out.insert(out.end(), in.begin() + f.begin, in.begin() + f.end);
}

inline Bytes EconItem(uint64_t id, uint32_t account, uint32_t definition) {
    Bytes item;
    Integer(item, 1, id); Integer(item, 2, account);
    // Zero means unacknowledged: every SO update would reopen Dota's new-item
    // dialog for the entire catalog. One item per definition gives stable slots.
    Integer(item, 3, definition);
    Integer(item, 4, definition); Integer(item, 5, 1); // quantity
    Integer(item, 6, 1); Integer(item, 7, 4); // level, standard item quality
    return item; // Rarity is defined by the item schema, not the quality field.
}
inline Bytes ItemFields(const std::vector<uint32_t>& definitions, uint32_t account, uint64_t firstId) {
    Bytes items;
    for (uint32_t definition : definitions) Blob(items, 2, EconItem(firstId++, account, definition));
    return items;
}
inline Bytes Packet(uint32_t type, const Bytes& body) {
    Bytes out; Write32(out, type | ProtoFlag); Write32(out, 0);
    out.insert(out.end(), body.begin(), body.end()); return out;
}
inline Bytes RefreshPacket(uint64_t steamId) {
    Bytes owner, body;
    Integer(owner, 1, 1); Integer(owner, 2, steamId); // CMsgSOIDOwner: Steam account
    Blob(body, 2, owner); // CMsgSOCacheSubscriptionRefresh.owner_soid
    return Packet(CacheRefresh, body);
}

enum class PatchState { Unchanged, Modified, Invalid, NoLocalCache };
struct Patch {
    PatchState state = PatchState::Unchanged;
    Bytes bytes;
};
inline Patch PatchCache(const Bytes& cache, uint64_t steamId, const Bytes& items) {
    std::vector<Field> fields;
    if (!Parse(cache, fields)) return { PatchState::Invalid, {} };
    uint64_t ownerId = 0, ownerType = 0;
    for (const auto& f : fields) {
        if (f.number != 4 || f.wire != 2) continue;
        auto owner = Payload(cache, f); std::vector<Field> ownerFields;
        if (!Parse(owner, ownerFields)) return { PatchState::Invalid, {} };
        for (const auto& of : ownerFields) {
            if (of.number == 1 && of.wire == 0) ownerType = of.value;
            if (of.number == 2 && of.wire == 0) ownerId = of.value;
        }
    }
    if (!steamId || ownerId != steamId || ownerType != 1) return { PatchState::NoLocalCache, {} };
    Bytes out; bool inserted = false;
    for (const auto& f : fields) {
        if (f.number == 2 && f.wire == 2) {
            auto group = Payload(cache, f); std::vector<Field> groupFields;
            if (!Parse(group, groupFields)) return { PatchState::Invalid, {} };
            uint64_t type = 0;
            for (const auto& gf : groupFields) if (gf.number == 1 && gf.wire == 0) type = gf.value;
            if (type == 1 && !inserted) {
                group.insert(group.end(), items.begin(), items.end());
                Blob(out, 2, group); inserted = true; continue;
            }
        }
        CopyField(out, cache, f);
    }
    if (!inserted) {
        Bytes group; Integer(group, 1, 1);
        group.insert(group.end(), items.begin(), items.end());
        Blob(out, 2, group);
    }
    if (out.size() > MaxPacket) return { PatchState::Invalid, {} };
    return { PatchState::Modified, std::move(out) };
}
template <typename CachePatcher>
inline Patch PatchPacketWith(uint32_t type, const void* data, size_t size, CachePatcher patchCache) {
    const uint32_t kind = type & ~ProtoFlag;
    if (!(type & ProtoFlag) || (kind != CacheSubscribed && kind != ClientWelcome)) return {};
    if (!data || size < 8 || size > MaxPacket) return { PatchState::Invalid, {} };
    const auto* p = static_cast<const uint8_t*>(data);
    if (Read32(p) != type || Read32(p + 4) > size - 8) return { PatchState::Invalid, {} };
    const size_t bodyAt = 8 + size_t(Read32(p + 4));
    Bytes body(p + bodyAt, p + size), changed;
    if (kind == CacheSubscribed) {
        auto result = patchCache(body);
        if (result.state != PatchState::Modified) return result;
        changed = std::move(result.bytes);
    } else {
        std::vector<Field> fields;
        if (!Parse(body, fields)) return { PatchState::Invalid, {} };
        bool patched = false;
        for (const auto& f : fields) {
            if (f.number == 3 && f.wire == 2) { // Welcome wraps caches in field 3.
                auto result = patchCache(Payload(body, f));
                if (result.state == PatchState::Invalid) return result;
                if (result.state == PatchState::Modified) {
                    Blob(changed, 3, result.bytes); patched = true; continue;
                }
            }
            CopyField(changed, body, f);
        }
        if (!patched) return { PatchState::NoLocalCache, {} };
    }
    if (changed.size() > MaxPacket - bodyAt) return { PatchState::Invalid, {} };
    Bytes out(p, p + bodyAt); // Preserve the complete protobuf routing header.
    out.insert(out.end(), changed.begin(), changed.end());
    return { PatchState::Modified, std::move(out) };
}
inline Patch PatchPacket(uint32_t type, const void* data, size_t size, uint64_t steamId, const Bytes& items) {
    if (items.empty()) return {};
    return PatchPacketWith(type, data, size, [&](const Bytes& cache) { return PatchCache(cache, steamId, items); });
}

// Retain a grown packet until the caller supplies enough space. Steam's API
// requires BufferTooSmall to leave the message at the head of the receive queue.
struct PendingPacket {
    uint32_t type = 0;
    Bytes bytes;
    bool Empty() const { return bytes.empty(); }
    Result Deliver(uint32_t* messageType, void* dest, uint32_t capacity, uint32_t* size) {
        if (!messageType || !size) return InvalidMessage;
        *messageType = type; *size = uint32_t(bytes.size());
        if (!dest || capacity < bytes.size()) return BufferTooSmall;
        std::memcpy(dest, bytes.data(), bytes.size()); bytes.clear(); return OK;
    }
};

struct RefreshWindow {
    static constexpr uint32_t MaxAttempts = 3;
    static constexpr uint64_t RetryMs = 5000, TimeoutMs = 20000;
    uint64_t start = 0, next = 0;
    uint32_t attempts = 0;
    void Reset(uint64_t now) { start = next = now; attempts = 0; }
    bool Expired(uint64_t now) const { return now - start >= TimeoutMs; }
    bool Due(uint64_t now) const { return attempts < MaxAttempts && now >= next && !Expired(now); }
    void Sent(uint64_t now) { ++attempts; next = now + RetryMs; }
};
}
