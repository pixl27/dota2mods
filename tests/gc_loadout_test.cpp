#include "gc_loadout.h"
#include <iostream>
#include <stdexcept>

using gc::Bytes;
static int checks = 0;
static void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
// Independently encoded wire fixtures: hero 1, slot 0, real item 9; version 42.
const Bytes realItem{8,9,32,99,0x92,1,4,8,1,16,0,0x62,2,8,77};
static Bytes Cache() {
    Bytes cache{0x22,4,8,1,16,123,0x19,42,0,0,0,0,0,0,0,0x28,1};
    Bytes group{8,1}; gc::Blob(group, 2, realItem); gc::Blob(cache, 2, group); return cache;
}
static bool Equipped(const Bytes& item, uint32_t hero, uint32_t slot) {
    std::vector<gc::Field> fields; gc::Parse(item, fields);
    for (const auto& f : fields) if (f.number == 18 && f.wire == 2) {
        auto state = gc::Payload(item, f);
        if (gc::Value(state, 1) == hero && gc::Value(state, 2) == slot) return true;
    }
    return false;
}
static gc::EquipResult Equip(gc::LocalLoadout& loadout, const Bytes& entry, bool batch = true) {
    Bytes body; if (batch) gc::Blob(body, 1, entry); else body = entry;
    // Source job = 99; response must route to target job = 99.
    const Bytes header{0x51,99,0,0,0,0,0,0,0};
    auto type = gc::ProtoFlag | (batch ? gc::EquipItems : gc::AdjustEquipped);
    auto packet = gc::RoutedPacket(type, body, header);
    return loadout.Equip(type, packet.data(), packet.size());
}
int main() {
    try {
        gc::LocalLoadout services; services.Reset({42},123,7);
        const Bytes nonEcon{0x22,4,8,1,16,123,0x28,0,0x12,2,8,0x7f};
        Check(services.RewriteCache(nonEcon).state == gc::PatchState::NoLocalCache,
            "A service-0 cache arriving first must not receive the entire cosmetic catalog");
        Check(services.RewriteCache({0x22,4,8,1,16,123}).state == gc::PatchState::NoLocalCache,
            "An unspecified cache without an econ group must not count as inventory delivery");
        const Bytes emptyEcon{0x22,4,8,1,16,123,0x28,1};
        Check(services.RewriteCache(emptyEcon).state == gc::PatchState::Modified,
            "An empty service-1 inventory can still acquire local cosmetics");
        Check(services.RewriteCache(nonEcon).state == gc::PatchState::NoLocalCache,
            "A later service-0 cache must not overwrite the econ service");
        // Disk serialization is different from a live subscription: caches in
        // field 2, TypeCache in field 4, econ service in field 3.
        Bytes diskGroup{8,1}; gc::Blob(diskGroup,2,gc::EconItem(1ULL<<62,123,42));
        gc::Blob(diskGroup,2,gc::EconItem(99,123,99)); gc::Integer(diskGroup,3,1);
        Bytes diskCache{8,1,16,123}; gc::Blob(diskCache,4,diskGroup);
        Bytes disk{8,4}; gc::Blob(disk,2,diskCache);
        auto persisted = gc::InspectDiskInventory(disk,123);
        Check(persisted.valid && persisted.items == 2 && persisted.localItems == 1,
            "Detect the saved local item that can reappear without loading Wardrobe");
        Check(gc::InspectDiskInventory(disk,124).localItems == 0,
            "Disk-cache diagnostics must match the exact account");
        Check(!gc::InspectDiskInventory({8,4,18,127},123).valid,
            "Truncated disk caches are not treated as known inventory state");
        auto savedItem = gc::EconItem(1ULL<<62,123,42);
        gc::Integer(savedItem,15,2); gc::Blob(savedItem,18,{8,49,16,0});
        Bytes savedGroup{8,1}; gc::Blob(savedGroup,2,savedItem); gc::Integer(savedGroup,3,1);
        Bytes savedCache{8,1,16,123}; gc::Blob(savedCache,4,savedGroup);
        Bytes savedDisk{8,4}; gc::Blob(savedDisk,2,savedCache);
        gc::LocalLoadout restored; restored.Reset({42,43},123,(1ULL<<62)+100);
        Check(restored.RestoreDiskSelections(savedDisk) && Equipped(restored.Item((1ULL<<62)+100),49,0) &&
            gc::Value(restored.Item((1ULL<<62)+100),15) == 2,
            "Restore saved hero/slot/style using current catalog IDs rather than stale disk IDs");
        const auto restoredAppearance = restored.Appearance(77);
        Check(restoredAppearance->revision == 77 && restoredAppearance->steamId == 123 && restoredAppearance->selections.size() == 1 &&
            restoredAppearance->selections[0].definition == 42, "Publish the restored native appearance without scanning the full catalog per frame");
        gc::LocalLoadout otherAccount; otherAccount.Reset({42},124,500);
        otherAccount.RestoreDiskSelections(savedDisk);
        Check(otherAccount.Appearance(0)->selections.empty(), "Another account's saved selection must not be restored");
        Check(!restored.RestoreDiskSelections({8,4,18,127}) && restored.Appearance(0)->selections[0].item == (1ULL<<62)+100,
            "A malformed disk cache must not clear a valid outfit");
        Bytes priorGroup{8,1}; gc::Blob(priorGroup,2,savedItem);
        Bytes priorCache{0x22,4,8,1,16,123,0x28,1}; gc::Blob(priorCache,2,priorGroup);
        auto currentCache = restored.RewriteCache(priorCache);
        std::vector<gc::Field> cacheFields; gc::Parse(currentCache.bytes,cacheFields);
        size_t count = 0; bool oldIdFound = false;
        for (const auto& c : cacheFields) if (c.number == 2 && c.wire == 2) {
            const auto group = gc::Payload(currentCache.bytes,c); std::vector<gc::Field> groupFields; gc::Parse(group,groupFields);
            for (const auto& i : groupFields) if (i.number == 2 && i.wire == 2) {
                ++count; oldIdFound |= gc::Value(gc::Payload(group,i),1) == (1ULL<<62);
            }
        }
        Check(count == 2 && !oldIdFound, "A refresh replaces the previous preview catalog instead of duplicating old IDs");
        gc::LocalLoadout loadout; loadout.Reset({42,43}, 123, 7);
        auto patched = loadout.RewriteCache(Cache());
        Check(patched.state == gc::PatchState::Modified && loadout.Item(9) == realItem,
            "Observe owned items without changing attributes or initial equipped state");
        auto result = Equip(loadout, {8,7,16,1,24,0});
        Check(result.state == gc::EquipState::Handled && result.count == 1 && result.replies.size() == 2,
            "A native batched Equip produces both a cache update and job response");
        Check(Equipped(loadout.Item(7),1,0) && !Equipped(loadout.Item(9),1,0),
            "Equip the local item in slot zero and clear the prior owned item locally");
        Bytes body, header;
        auto& update = result.replies[0];
        Check(gc::Unpack(update.type, update.bytes.data(), update.bytes.size(), body, header) && update.type == (gc::ProtoFlag | 26),
            "Return a correctly framed SOUpdateMultiple");
        const Bytes expectedUpdate{
            0x12,12,8,1,18,8,8,9,32,99,0x62,2,8,77,
            0x12,25,8,1,18,21,8,7,16,123,24,42,32,42,40,1,48,1,56,4,0x92,1,4,8,1,16,0,
            0x19,42,0,0,0,0,0,0,0,0x32,4,8,1,16,123,0x38,1};
        Check(body == expectedUpdate, "Changed objects, owner, service and fixed64 version match the Dota wire schema exactly");
        const auto& reply = result.replies[1];
        Check(gc::Unpack(reply.type, reply.bytes.data(), reply.bytes.size(), body, header) &&
            header == Bytes{0x59,99,0,0,0,0,0,0,0,0x68,1} && body == Bytes{9,42,0,0,0,0,0,0,0},
            "Acknowledge the originating job and the same cache version after the update");
        Check(Equip(loadout,{8,8,16,1,24,0,32,2},false).state == gc::EquipState::Handled &&
            Equipped(loadout.Item(8),1,0) && !Equipped(loadout.Item(7),1,0) && gc::Value(loadout.Item(8),15) == 2,
            "Legacy single equips replace the previous local item and apply the requested style");
        result = Equip(loadout,{8,8,16,1,24,0});
        Check(result.state == gc::EquipState::Handled && result.replies.size() == 2,
            "Repeated clicks still complete the client job");
        auto refresh = loadout.RewriteCache(Cache());
        Check(refresh.state == gc::PatchState::Modified && Equipped(loadout.Item(8),1,0) && !Equipped(loadout.Item(9),1,0),
            "A later server cache refresh preserves local selections");
        auto repeated = loadout.RewriteCache(refresh.bytes);
        Check(repeated.bytes == refresh.bytes, "Repeated refresh must not append duplicate local inventory IDs");
        Check(gc::Value(loadout.Item(7),3) == 42 && gc::Value(loadout.Item(8),3) == 43,
            "Equip replies and refreshes keep catalog items acknowledged instead of reopening all new-item notifications");
        Check(Equip(loadout,{8,0,16,1,24,0}).state == gc::EquipState::Handled && !Equipped(loadout.Item(8),1,0),
            "Default/unequip clears the local slot");
        Check(Equip(loadout,{8,7,16,1,24,0}).state == gc::EquipState::Handled &&
            Equip(loadout,{8,7,16,1,24,255,255,3}).state == gc::EquipState::Handled && !Equipped(loadout.Item(7),1,0),
            "The 65535 unequip slot clears the selected item");
        gc::LocalLoadout clean; clean.Reset({42,43},123,7); clean.RewriteCache(Cache());
        Check(Equip(clean,{8,9,16,1,24,0}).state == gc::EquipState::PassThrough,
            "A normal owned-item equip stays on Steam's original path");
        auto other = Cache(); other[5] = 124;
        Check(clean.RewriteCache(other).state == gc::PatchState::NoLocalCache,
            "Never observe or rewrite another account's cache");
        Bytes entries; gc::Blob(entries,1,{8,7,16,1,24,0}); gc::Blob(entries,1,{8,8,16,1,24,1});
        auto packet = gc::Packet(gc::EquipItems,entries);
        result = clean.Equip(gc::ProtoFlag | gc::EquipItems,packet.data(),packet.size());
        Check(result.state == gc::EquipState::Handled && result.count == 2 && Equipped(clean.Item(7),1,0) && Equipped(clean.Item(8),1,1),
            "A full-set batch updates all slots in one transaction");
        auto before = clean.Item(7);
        entries.clear(); gc::Blob(entries,1,{8,7,16,1,24,1}); gc::Blob(entries,1,{8,100,16,1,24,0});
        packet = gc::Packet(gc::EquipItems,entries);
        Check(clean.Equip(gc::ProtoFlag | gc::EquipItems,packet.data(),packet.size()).state == gc::EquipState::Invalid && clean.Item(7) == before,
            "Reject an unknown item in a local batch without applying the first entry");
        Check(Equip(clean,{8,7}).state == gc::EquipState::Invalid && clean.Item(7) == before,
            "Malformed equip requests cannot silently select hero zero or mutate state");
        packet = gc::Packet(gc::EquipItems,{10,127});
        Check(clean.Equip(gc::ProtoFlag | gc::EquipItems,packet.data(),packet.size()).state == gc::EquipState::Invalid,
            "Reject truncated nested equip entries");
        auto malformed = Cache(); malformed.push_back(0);
        Check(clean.RewriteCache(malformed).state == gc::PatchState::Invalid && clean.Item(7) == before,
            "A malformed cache must not corrupt local selections");
        Check(Equip(clean,{8,9,16,1,24,0}).state == gc::EquipState::Handled && Equipped(clean.Item(9),1,0) && !Equipped(clean.Item(7),1,0),
            "An owned item can replace a local preview in the same slot");
        gc::LocalLoadout live; live.Reset({42,43},123,7); live.RewriteCache(Cache());
        const Bytes newlyEquipped{8,10,32,100,0x92,1,4,8,1,16,0,0x62,2,8,88};
        Bytes delta{16,1}; gc::Blob(delta,3,newlyEquipped);
        gc::Fixed64(delta,4,50); gc::Blob(delta,5,{8,1,16,123}); gc::Integer(delta,6,1);
        packet = gc::Packet(22,delta);
        Check(live.RewritePacket(gc::ProtoFlag|22,packet.data(),packet.size()).state == gc::PatchState::Unchanged && live.Item(10) == newlyEquipped,
            "Observe a genuine Steam equip without changing its packet");
        result = Equip(live,{8,7,16,1,24,0});
        Check(result.state == gc::EquipState::Handled && !Equipped(live.Item(10),1,0),
            "A local preview clears an owned item equipped since the initial refresh");
        auto updated = live.RewritePacket(gc::ProtoFlag|22,packet.data(),packet.size());
        Check(updated.state == gc::PatchState::Modified &&
            gc::Unpack(gc::ProtoFlag|22,updated.bytes.data(),updated.bytes.size(),body,header),
            "A later real SO update retains the active local preview");
        std::vector<gc::Field> deltaFields; gc::Parse(body,deltaFields);
        for (const auto& f : deltaFields) if (f.number == 3 && f.wire == 2)
            Check(!Equipped(gc::Payload(body,f),1,0) && gc::Value(gc::Payload(body,f),4) == 100,
                "Reconcile only equipped state while retaining real item definition and attributes");
        auto otherService = Cache(); otherService[16] = 6;
        Check(live.RewriteCache(otherService).state == gc::PatchState::NoLocalCache,
            "Another local GC service must not replace the econ cache's version or inventory");
        result = Equip(live,{8,8,16,1,24,1});
        const auto& liveReply = result.replies.back();
        Check(gc::Unpack(liveReply.type,liveReply.bytes.data(),liveReply.bytes.size(),body,header) && gc::Value(body,1) == 50,
            "Equip acknowledgements follow the latest real econ cache version");
        Bytes removed{16,1}; gc::Blob(removed,3,{8,10}); gc::Fixed64(removed,4,51);
        gc::Blob(removed,5,{8,1,16,123}); gc::Integer(removed,6,1); packet = gc::Packet(23,removed);
        Check(live.RewritePacket(gc::ProtoFlag|23,packet.data(),packet.size()).state == gc::PatchState::Unchanged && live.Item(10).empty(),
            "Removing a real item must remove it from the observed inventory");
        Bytes multiple, object{8,1}; gc::Blob(object,2,newlyEquipped); gc::Blob(multiple,4,object);
        gc::Fixed64(multiple,3,52); gc::Blob(multiple,6,{8,1,16,123}); gc::Integer(multiple,7,1);
        packet = gc::Packet(26,multiple);
        Check(live.RewritePacket(gc::ProtoFlag|26,packet.data(),packet.size()).state == gc::PatchState::Modified && !live.Item(10).empty(),
            "Batched Steam additions are observed and reconciled with the local loadout");
        std::vector<uint32_t> definitions;
        for (uint32_t i = 1; i <= 15000; ++i) definitions.push_back(i);
        gc::LocalLoadout large; large.Reset(definitions,123,1ULL<<62); large.RewriteCache(Cache());
        Bytes selected; gc::Integer(selected,1,(1ULL<<62)+14999); gc::Integer(selected,2,1); gc::Integer(selected,3,0);
        result = Equip(large,selected);
        Check(result.state == gc::EquipState::Handled && result.replies[0].bytes.size() < 256 && Equipped(large.Item((1ULL<<62)+14999),1,0),
            "A 15,000-item catalog uses full 64-bit IDs and emits only the changed item records per click");
        std::cout << "PASS: " << checks << " local equip, loadout and acknowledgement checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
