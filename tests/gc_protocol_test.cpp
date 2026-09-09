#include "gc_protocol.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>

using gc::Bytes;
static int checks = 0;
static void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
// Hand-written schema fixtures, independent of the production writer:
// owner_soid={type:1,id:123}; version=42; existing econ item={id:9,def_index:99}.
const Bytes cache = {0x22,4,8,1,16,123, 0x19,42,0,0,0,0,0,0,0,
    0x12,8,8,1,18,4,8,9,32,99};
const Bytes itemFields = {18,14,8,7,16,123,24,42,32,42,40,1,48,1,56,4};

int main() {
    try {
        Check(gc::EconItem(7, 123, 42) == Bytes(itemFields.begin() + 2, itemFields.end()),
            "Econ item wire fields must match the Dota schema");
        Check(gc::ItemFields({42}, 123, 7) == itemFields, "SubscribedType items use field 2");
        const Bytes refresh = {28,0,0,128,0,0,0,0,18,4,8,1,16,123};
        Check(gc::RefreshPacket(123) == refresh, "Late attachment must request the local SO cache");

        Bytes expected = {0x22,4,8,1,16,123,0x19,42,0,0,0,0,0,0,0,
            0x12,24,8,1,18,4,8,9,32,99};
        expected.insert(expected.end(), itemFields.begin(), itemFields.end());
        auto patchedCache = gc::PatchCache(cache, 123, itemFields);
        Check(patchedCache.state == gc::PatchState::Modified && patchedCache.bytes == expected,
            "Keep existing items and cache metadata; extend the existing econ group");
        Check(gc::PatchCache(cache, 124, itemFields).state == gc::PatchState::NoLocalCache,
            "Never modify another account's cache");
        Check(gc::PatchCache(Bytes{0x22,4,8,2,16,123}, 123, itemFields).state == gc::PatchState::NoLocalCache,
            "Owner type must be an account");
        Check(gc::PatchCache(Bytes{0x22,4,8,1,16,123}, 123, itemFields).state == gc::PatchState::Modified,
            "An empty local inventory can acquire an econ group");

        // Welcome includes game_data and caches for two different accounts.
        Bytes welcome = {8,1,18,3,'a','b','c',26,uint8_t(cache.size())};
        welcome.insert(welcome.end(), cache.begin(), cache.end());
        Bytes other = cache; other[5] = 124;
        welcome.push_back(26); welcome.push_back(uint8_t(other.size()));
        welcome.insert(welcome.end(), other.begin(), other.end());
        // Nonempty routing header is preserved byte for byte.
        Bytes packet = {0xa4,0x0f,0,0x80,2,0,0,0,8,1};
        packet.insert(packet.end(), welcome.begin(), welcome.end());
        auto patched = gc::PatchPacket(gc::ProtoFlag | 4004, packet.data(), packet.size(), 123, itemFields);
        Bytes expectedPacket(packet.begin(), packet.begin() + 17);
        expectedPacket.push_back(26); expectedPacket.push_back(uint8_t(expected.size()));
        expectedPacket.insert(expectedPacket.end(), expected.begin(), expected.end());
        expectedPacket.push_back(26); expectedPacket.push_back(uint8_t(other.size()));
        expectedPacket.insert(expectedPacket.end(), other.begin(), other.end());
        Check(patched.state == gc::PatchState::Modified && patched.bytes == expectedPacket,
            "Welcome must rewrite nested field 3 and preserve all other bytes");
        auto standalone = gc::Packet(gc::CacheSubscribed, cache);
        auto direct = gc::PatchPacket(gc::CacheSubscribed | gc::ProtoFlag, standalone.data(), standalone.size(), 123, itemFields);
        Check(direct.state == gc::PatchState::Modified && direct.bytes == gc::Packet(gc::CacheSubscribed, expected),
            "Direct cache subscription packets must work too");
        Check(gc::PatchPacket(gc::ProtoFlag | 4009, packet.data(), packet.size(), 123, itemFields).state == gc::PatchState::Unchanged,
            "Unrelated GC packets must pass through");
        Check(gc::PatchPacket(4004, packet.data(), packet.size(), 123, itemFields).state == gc::PatchState::Unchanged,
            "Do not parse non-protobuf messages");
        Check(gc::PatchPacket(gc::ProtoFlag | 4004, packet.data(), packet.size(), 123, {}).state == gc::PatchState::Unchanged,
            "An empty catalog must never claim to inject inventory");
        auto noCache = gc::Packet(4004, {8,1});
        Check(gc::PatchPacket(gc::ProtoFlag | 4004, noCache.data(), noCache.size(), 123, itemFields).state == gc::PatchState::NoLocalCache,
            "Welcome without a full cache does not count as success");

        auto badHeader = packet; badHeader[4] = 255;
        Check(gc::PatchPacket(gc::ProtoFlag | 4004, badHeader.data(), badHeader.size(), 123, itemFields).state == gc::PatchState::Invalid,
            "Reject a routing header extending past the packet");
        Check(gc::PatchPacket(gc::ProtoFlag | 4004, packet.data(), 7, 123, itemFields).state == gc::PatchState::Invalid,
            "Reject a truncated packet header");
        Check(gc::PatchPacket(gc::ProtoFlag | 24, packet.data(), packet.size(), 123, itemFields).state == gc::PatchState::Invalid,
            "Message kind must agree with the packet header");
        std::vector<gc::Field> fields;
        for (const auto& invalid : std::vector<Bytes>{{0}, {18,127,1}, {9,1}, {13,1}, {11},
                {8,128,128,128,128,128,128,128,128,128,2}}) {
            Check(!gc::Parse(invalid, fields), "Malformed fields must be rejected without an out-of-bounds read");
        }
        auto invalidCache = gc::Packet(24, {0x22,127,8});
        auto saved = invalidCache;
        Check(gc::PatchPacket(gc::ProtoFlag | 24, invalidCache.data(), invalidCache.size(), 123, itemFields).state == gc::PatchState::Invalid
            && invalidCache == saved, "Invalid incoming bytes must remain untouched");

        gc::PendingPacket pending{gc::ProtoFlag | 24, direct.bytes};
        uint32_t type = 0, size = 0; Bytes small(4, 0xcd);
        Check(pending.Deliver(&type, small.data(), uint32_t(small.size()), &size) == gc::BufferTooSmall,
            "Report BufferTooSmall when a patched inventory outgrows Dota's buffer");
        Check(!pending.Empty() && size == direct.bytes.size() && small == Bytes(4, 0xcd),
            "Retain the message and leave a small destination untouched");
        Check(pending.Deliver(&type, nullptr, size, &size) == gc::BufferTooSmall && !pending.Empty(),
            "A size-only query must not consume the packet");
        Bytes enough(size + 8, 0xcd);
        Check(pending.Deliver(&type, enough.data(), size, &size) == gc::OK && pending.Empty(),
            "A successful retry consumes exactly one pending packet");
        Check(std::equal(direct.bytes.begin(), direct.bytes.end(), enough.begin()) && enough.back() == 0xcd,
            "A successful copy must not write beyond the packet");

        gc::RefreshWindow window; window.Reset(1000);
        Check(window.Due(1000), "The first refresh must be immediate");
        window.Sent(1000); Check(!window.Due(5999) && window.Due(6000), "Refresh attempts must be spaced out");
        window.Sent(6000); window.Sent(11000);
        Check(!window.Due(16000) && window.attempts == 3, "Stop after three requests");
        Check(!window.Expired(20999) && window.Expired(21000), "An absent GC reply must time out");
        window.Reset(30000); Check(window.Due(30000) && window.attempts == 0, "Retry starts a new bounded refresh");

        // Exercise length boundaries with a realistic large catalog.
        std::vector<uint32_t> definitions;
        for (uint32_t i = 1; i <= 15000; ++i) definitions.push_back(i);
        auto many = gc::ItemFields(definitions, 123, 1ULL << 62);
        auto large = gc::PatchPacket(gc::ProtoFlag | 24, standalone.data(), standalone.size(), 123, many);
        Check(large.state == gc::PatchState::Modified && large.bytes.size() > 300000,
            "Large catalogs must handle multi-byte protobuf lengths");
        std::mt19937 random(570);
        for (int i = 0; i < 2000; ++i) {
            Bytes bytes(random() % 160);
            for (auto& byte : bytes) byte = uint8_t(random());
            auto fuzz = gc::Packet(24, bytes);
            auto before = fuzz;
            gc::PatchPacket(gc::ProtoFlag | 24, fuzz.data(), fuzz.size(), 123, itemFields);
            Check(fuzz == before, "Packet validation must not mutate input");
        }
        std::cout << "PASS: " << checks << " GC protocol, buffering, retry and malformed-input checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
