// Run the production receiver against a local fake GC. No game process,
// Steam session, network messages, or actual detours are used by these tests.
#include "../src/inventory.cpp"
#include "../src/wardrobe_session.h"
#include <deque>
#include <iostream>
#include <stdexcept>

Config g_Cfg;
std::vector<SkinEntry> g_DB;
std::mutex g_DbMutex;
std::map<std::string, std::map<std::string, int>> g_Loadout;
std::mutex g_LoadoutMutex;

static void* failEnable = nullptr;
static void* failDisable = nullptr;
static int disableCalls = 0;
MH_STATUS WINAPI MH_Initialize() { return MH_OK; }
MH_STATUS WINAPI MH_CreateHook(void*, void*, void**) { return MH_OK; }
MH_STATUS WINAPI MH_RemoveHook(void*) { return MH_OK; }
MH_STATUS WINAPI MH_EnableHook(void* target) { return target == failEnable ? MH_ERROR_MEMORY_PROTECT : MH_OK; }
MH_STATUS WINAPI MH_DisableHook(void* target) { ++disableCalls; return target == failDisable ? MH_ERROR_MEMORY_PROTECT : MH_OK; }
const char* WINAPI MH_StatusToString(MH_STATUS) { return "MOCK_MH_ERROR"; }

struct Message { uint32_t type; gc::Bytes bytes; };
static std::deque<Message> incoming;
static gc::Result sendResult = gc::OK;
static gc::Bytes lastSent;
static uint32_t lastSentType = 0;
static int originalReads = 0, sends = 0, checks = 0, fakeCoordinator = 0;
struct CallbackEvent { int user = 123, kind = 1701; uint32_t size = 0; };
static std::deque<CallbackEvent> callbacks;
static int callbackReads = 0, callbackFrees = 0;
static std::vector<uint32_t> dispatched;
static void Check(bool ok, const char* message) {
    ++checks; if (!ok) throw std::runtime_error(message);
}
static bool __fastcall Available(void*, uint32_t* size) {
    if (incoming.empty()) return false;
    if (size) *size = uint32_t(incoming.front().bytes.size()); return true;
}
static gc::Result __fastcall Retrieve(void*, uint32_t* type, void* dest, uint32_t capacity, uint32_t* size) {
    ++originalReads;
    if (incoming.empty()) return gc::NoMessage;
    if (!type || !size) return gc::InvalidMessage;
    const auto& message = incoming.front();
    *type = message.type; *size = uint32_t(message.bytes.size());
    if (!dest || capacity < *size) return gc::BufferTooSmall;
    memcpy(dest, message.bytes.data(), *size); incoming.pop_front(); return gc::OK;
}
static gc::Result __fastcall Send(void*, uint32_t type, const void* data, uint32_t size) {
    ++sends; lastSentType = type;
    lastSent.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    return sendResult;
}
static bool __cdecl Callback(int, gc::SteamCallback* callback, void*) {
    ++callbackReads;
    if (callbacks.empty() || !callback) return false;
    auto& event = callbacks.front();
    *callback = {event.user, event.kind, reinterpret_cast<uint8_t*>(&event.size), sizeof(event.size)};
    return true;
}
static void __cdecl FreeCallback(int) {
    ++callbackFrees;
    if (!callbacks.empty()) callbacks.pop_front();
}
// Like a callback-driven game: never poll the GC until a message-available
// event arrives, and read only one packet for each event. No later click or
// network message is provided to rescue a stalled local queue.
static int Pump(bool consume = true) {
    gc::SteamCallback callback; int count = 0;
    while (OnCallback(g_SteamPipe, &callback, nullptr)) {
        Check(++count < 256, "A dispatch pump must finish; ignored notifications must not spin");
        if (consume && callback.callback == 1701) {
            uint32_t size = 0, type = 0; memcpy(&size, callback.data, sizeof(size));
            gc::Bytes dest(size);
            auto result = OnRetrieve(g_Coordinator, &type, dest.data(), uint32_t(dest.size()), &size);
            if (result == gc::BufferTooSmall) {
                dest.resize(size); result = OnRetrieve(g_Coordinator, &type, dest.data(), uint32_t(dest.size()), &size);
            }
            Check(result == gc::OK, "Each notification must make its packet readable");
            dispatched.push_back(type & ~gc::ProtoFlag);
        }
        OnFreeCallback(g_SteamPipe);
    }
    return count;
}
static void Reset() {
    appearance::Publish(nullptr); g_AppearanceRevision = 0;
    g_LogPath = "build/gc_receiver_test.log";
    g_InventoryStatus = {}; g_InventoryStatus.items = 1;
    g_Command = 0; g_Transform = false; g_ReceiveFailed = false; g_DeliveredAt = 0; g_ReserveInventory = true;
    g_Pending = {}; g_PendingInventory = false; g_LocalReplies.clear(); g_Setup = true;
    g_AvailableEnabled = g_RetrieveEnabled = g_SendEnabled = g_Waiting = g_StopRequested = false;
    g_CallbackEnabled = g_FreeCallbackEnabled = false;
    g_CallbackWake = {}; g_QueueSequence = g_PendingSequence = 0; g_SteamPipe = 1; g_SteamUser = 123;
    g_Coordinator = &fakeCoordinator; g_SteamId = 123;
    g_AvailableTarget = reinterpret_cast<void*>(1); g_RetrieveTarget = reinterpret_cast<void*>(2);
    g_SendTarget = reinterpret_cast<void*>(3);
    g_CallbackTarget = reinterpret_cast<void*>(4); g_FreeCallbackTarget = reinterpret_cast<void*>(5);
    g_OriginalAvailable = Available; g_OriginalRetrieve = Retrieve; g_Send = Send;
    g_OriginalCallback = Callback; g_OriginalFreeCallback = FreeCallback;
    g_ItemFields = gc::ItemFields({42}, 123, 7);
    g_LocalLoadout.Reset({42}, 123, 7);
    failEnable = failDisable = nullptr; disableCalls = originalReads = sends = 0;
    sendResult = gc::OK; incoming.clear(); lastSent.clear();
    callbacks.clear(); dispatched.clear(); callbackReads = callbackFrees = 0;
}
static void Start() { RequestInventoryRefresh(); TickInventory(); }
static Message Inventory() {
    return {gc::ProtoFlag | 24, gc::Packet(24, {0x22,4,8,1,16,123,0x12,8,8,1,18,4,8,9,32,99})};
}
int main() {
    try {
        const auto testPid = GetCurrentProcessId();
        Check(!HasWardrobeSession(testPid), "A fresh process must not inherit a Wardrobe session marker");
        auto marker = ClaimWardrobeSession(testPid);
        Check(marker && HasWardrobeSession(testPid), "A loaded instance must be visible to the launcher");
        Check(!ClaimWardrobeSession(testPid) && HasWardrobeSession(testPid), "Reject duplicate initialization without releasing the existing instance");
        CloseHandle(marker);
        Check(!HasWardrobeSession(testPid), "Closing the lifetime marker must permit a fresh session");
        Reset(); Start();
        auto state = GetInventoryStatus();
        Check(state.phase == InventoryPhase::Waiting && state.hookActive && state.refreshes == 1,
            "Late attachment must arm the receiver and immediately refresh");
        Check(sends == 1 && lastSentType == (gc::ProtoFlag | 28) && lastSent == gc::RefreshPacket(123),
            "Send only the local cache refresh request");
        TickInventory(); Check(sends == 1, "Do not send a refresh every worker tick");
        g_Refresh.start = GetTickCount64() - gc::RefreshWindow::TimeoutMs;
        TickInventory();
        Check(GetInventoryStatus().phase == InventoryPhase::TimedOut, "No packets must produce a timeout instead of permanent waiting");
        Start(); Check(GetInventoryStatus().phase == InventoryPhase::Waiting && sends == 2, "Retry must recover after timeout");

        auto inventory = Inventory(); incoming.push_back(inventory);
        uint32_t type = 0, size = 0;
        Check(OnAvailable(g_Coordinator, &size) && size > inventory.bytes.size(), "Availability must reserve room for local records");
        gc::Bytes dest(size);
        Check(OnRetrieve(g_Coordinator, &type, dest.data(), uint32_t(dest.size()), &size) == gc::OK,
            "Receive and patch a full inventory");
        Check(size > inventory.bytes.size() && GetInventoryStatus().phase == InventoryPhase::Delivered,
            "Claim delivery only after copied bytes are returned to the game");
        TickInventory(); Check(g_Transform && !g_Waiting, "Stop refresh attempts while keeping local equips available");
        g_StopAt = 0; TickInventory();
        Check(GetInventoryStatus().hookActive && GetInventoryStatus().equipActive && disableCalls == 0,
            "Keep receiver and notification hooks active after delivery");
        const auto beforeRetry = sends;
        Start();
        Check(sends == beforeRetry && GetInventoryStatus().recordsDelivered && !g_Waiting,
            "Retry after delivery must resume the handler without reloading all 13k items");

        const auto equip = gc::Packet(gc::EquipItems, {10,6,8,7,16,1,24,0});
        const auto sentBefore = sends;
        Check(OnSend(g_Coordinator, gc::ProtoFlag | gc::EquipItems, equip.data(), uint32_t(equip.size())) == gc::OK && sends == sentBefore,
            "A local equip must never send synthetic item IDs to Steam");
        Check(GetInventoryStatus().localEquips == 1 && g_LocalReplies.size() == 2,
            "Native equip creates an update and response and reports progress");
        const auto initialAppearance = appearance::Read();
        Check(initialAppearance && initialAppearance->selections.empty(), "Queued equips must not yet reach the renderer");
        Check(OnAvailable(g_Coordinator, &size) && size == g_LocalReplies.front().bytes.size(),
            "The game can discover a local update even with no incoming Steam traffic");
        const auto expectedUpdate = g_LocalReplies.front().bytes;
        auto readsBefore = originalReads;
        Check(OnRetrieve(g_Coordinator, &type, nullptr, 0, &size) == gc::BufferTooSmall && g_LocalReplies.size() == 2,
            "A size query cannot consume a local equip update");
        incoming.push_back({gc::ProtoFlag | 4009, gc::Packet(4009, {8,0})});
        dest.assign(size, 0);
        Check(OnRetrieve(g_Coordinator, &type, dest.data(), uint32_t(dest.size()), &size) == gc::OK && type == (gc::ProtoFlag | 4009) &&
            g_LocalReplies.size() == 2 && incoming.empty(), "Real GC connection traffic must pass ahead of queued local updates");
        dest.assign(expectedUpdate.size(), 0);
        Check(OnRetrieve(g_Coordinator, &type, dest.data(), uint32_t(dest.size()), &size) == gc::OK && dest == expectedUpdate &&
            type == (gc::ProtoFlag | gc::UpdateMultiple) && GetInventoryStatus().equipUpdates == 1,
            "Deliver the equipped-state update and count it only after a successful copy");
        Check(originalReads > readsBefore && incoming.empty(), "Probe and drain real GC traffic before serving local replies");
        Check(appearance::Read() == initialAppearance, "An update without its acknowledgement must not publish a new appearance");
        PauseInventory(); TickInventory();
        Check(disableCalls == 0 && !GetInventoryStatus().equipActive,
            "Pausing stops new equips but keeps receive hooks until the acknowledgement drains");
        dest.assign(1024, 0);
        Check(OnRetrieve(g_Coordinator, &type, dest.data(), uint32_t(dest.size()), &size) == gc::OK && type == (gc::ProtoFlag | gc::EquipItemsResponse),
            "Deliver the equip job acknowledgement after its cache update");
        Check(appearance::Read()->selections.size() == 1 && appearance::Read()->selections[0].definition == 42,
            "Publish an immutable appearance only when its final reply is returned");
        TickInventory();
        Check(!GetInventoryStatus().hookActive && disableCalls == 5, "Pause all five hooks after the reply queue drains");

        Reset(); Start(); incoming.push_back(inventory); dest.assign(1024, 0);
        OnRetrieve(g_Coordinator, &type, dest.data(), uint32_t(dest.size()), &size);
        const auto ownedEquip = gc::Packet(gc::EquipItems, {10,6,8,9,16,1,24,0});
        Check(OnSend(g_Coordinator, gc::ProtoFlag | gc::EquipItems, ownedEquip.data(), uint32_t(ownedEquip.size())) == gc::OK &&
            sends == 2 && lastSent == ownedEquip && g_LocalReplies.empty(), "Owned-only equip requests reach Steam byte for byte");
        const auto invalidEquip = gc::Packet(gc::EquipItems, {10,127});
        Check(OnSend(g_Coordinator, gc::ProtoFlag | gc::EquipItems, invalidEquip.data(), uint32_t(invalidEquip.size())) == gc::InvalidMessage &&
            sends == 2 && !GetInventoryStatus().equipDetail.empty(), "Invalid equip requests surface an error and do not leak local IDs");

        Reset(); Start(); incoming.push_back(inventory);
        dest.assign(inventory.bytes.size(), 0);
        Check(OnRetrieve(g_Coordinator, &type, dest.data(), uint32_t(dest.size()), &size) == gc::BufferTooSmall,
            "A fixed-size caller must get the required expanded size");
        Check(GetInventoryStatus().phase == InventoryPhase::Waiting && !g_Pending.Empty(),
            "A pending oversized message is not delivered yet");
        const auto pendingBytes = g_Pending.bytes;
        incoming.push_back({gc::ProtoFlag | 4009, gc::Packet(4009, {8,0})});
        PauseInventory(); TickInventory();
        Check(disableCalls == 0 && GetInventoryStatus().hookActive, "Pause must wait for pending packet delivery");
        uint32_t pendingSize = 0;
        Check(OnAvailable(g_Coordinator, &pendingSize) && pendingSize == pendingBytes.size(), "Expose pending packet size before the next Steam message");
        dest.assign(pendingSize, 0);
        Check(OnRetrieve(g_Coordinator, &type, dest.data(), uint32_t(dest.size()), &size) == gc::OK && dest == pendingBytes,
            "Retry must deliver the retained packet unchanged");
        Check(originalReads == 1 && incoming.size() == 1, "Retry must not consume the next GC message");
        TickInventory(); g_StopAt = 0; TickInventory();
        Check(!GetInventoryStatus().hookActive, "Finish pausing once pending data is consumed");

        Reset(); Start(); incoming.push_back(inventory);
        int otherCoordinator = 0; dest.assign(1024, 0);
        Check(OnRetrieve(&otherCoordinator, &type, dest.data(), uint32_t(dest.size()), &size) == gc::OK
            && size == inventory.bytes.size() && GetInventoryStatus().packets == 0,
            "Other coordinator instances must pass through untouched");
        incoming.push_back({gc::ProtoFlag | 4009, gc::Packet(4009, {8,0})});
        const auto unchanged = incoming.front().bytes;
        Check(OnRetrieve(g_Coordinator, &type, dest.data(), uint32_t(dest.size()), &size) == gc::OK
            && gc::Bytes(dest.begin(), dest.begin() + size) == unchanged,
            "Other message types must pass through byte for byte");

        Reset(); Start(); incoming.push_back({gc::ProtoFlag | 24, gc::Packet(24, {0x22,127})});
        const auto malformed = incoming.front().bytes;
        Check(OnRetrieve(g_Coordinator, &type, dest.data(), uint32_t(dest.size()), &size) == gc::OK
            && gc::Bytes(dest.begin(), dest.begin() + size) == malformed,
            "Malformed inventory must still reach the game unchanged");
        Check(GetInventoryStatus().phase == InventoryPhase::Failed, "Invalid format must be visible in the HUD");
        TickInventory(); Check(!g_Waiting && !g_Transform, "Invalid formats must not trigger endless retries");

        Reset(); sendResult = gc::NotLoggedOn; Start();
        Check(GetInventoryStatus().phase == InventoryPhase::Failed && !g_Waiting, "Steam sign-in failure must not display waiting");
        Reset(); failEnable = g_RetrieveTarget; Start();
        Check(GetInventoryStatus().phase == InventoryPhase::Failed && !GetInventoryStatus().hookActive && sends == 0,
            "A failed receive hook must roll back and must not request inventory");
        Reset(); failEnable = g_SendTarget; Start();
        Check(GetInventoryStatus().phase == InventoryPhase::Failed && !GetInventoryStatus().hookActive && sends == 0 && disableCalls == 4,
            "A failed equip hook must roll back receiver and notification hooks before any refresh is sent");
        Reset(); failEnable = g_CallbackTarget; Start();
        Check(GetInventoryStatus().phase == InventoryPhase::Failed && !GetInventoryStatus().hookActive && sends == 0,
            "A failed notification hook must not leave a silently broken equip handler active");
        Reset(); failEnable = g_RetrieveTarget; failDisable = g_AvailableTarget; Start();
        Check(GetInventoryStatus().phase == InventoryPhase::Failed && GetInventoryStatus().hookActive,
            "A failed rollback must not falsely claim hooks are inactive");
        Reset(); g_Setup = false; Start();
        Check(GetInventoryStatus().phase == InventoryPhase::Failed && sends == 0,
            "Missing Steam API must be an explicit initialization failure");

        Reset(); Start(); incoming.push_back(inventory); dest.assign(1024,0);
        OnRetrieve(g_Coordinator,&type,dest.data(),uint32_t(dest.size()),&size); TickInventory();
        Check(!g_ReserveInventory, "Stop reserving the full catalog size for every normal GC packet after refresh");
        Check(OnSend(g_Coordinator,gc::ProtoFlag|gc::EquipItems,equip.data(),uint32_t(equip.size())) == gc::OK,
            "Accept an equip with no network activity");
        Check(Pump() == 2 && dispatched == std::vector<uint32_t>{26,2570} && g_LocalReplies.empty(),
            "One click must wake the game and deliver BOTH update and ack in the same callback pump");
        Check(callbackFrees == 0 && sends == 1 && GetInventoryStatus().completedEquips == 1 && GetInventoryStatus().equipAcks == 1,
            "Local notifications must neither free Steam events nor send network wakeup traffic");
        for (int i = 0; i < 10; ++i) {
            OnSend(g_Coordinator,gc::ProtoFlag|gc::EquipItems,equip.data(),uint32_t(equip.size()));
            Pump();
        }
        auto finished = GetInventoryStatus();
        Check(finished.acceptedEquips == 11 && finished.completedEquips == 11 && finished.equipUpdates == 11 && finished.equipAcks == 11,
            "Successive clicks must never remain one equip behind");
        Check(finished.wakeups == 22 && g_LocalReplies.empty(), "All rapid-click replies must drain without a later network event");

        OnSend(g_Coordinator,gc::ProtoFlag|gc::EquipItems,equip.data(),uint32_t(equip.size()));
        dispatched.clear();
        incoming.push_back({gc::ProtoFlag|4009,gc::Packet(4009,{8,1})});
        callbacks.push_back({123,1701,uint32_t(incoming.front().bytes.size())});
        Check(OnAvailable(g_Coordinator,&size) && size == incoming.front().bytes.size(),
            "Real messages keep their actual buffer size while local updates wait");
        Check(Pump() == 3 && dispatched == std::vector<uint32_t>{4009,26,2570} && callbackFrees == 1,
            "Deliver a real connection event first, then local update/ack without losing any callback");

        OnSend(g_Coordinator,gc::ProtoFlag|gc::EquipItems,equip.data(),uint32_t(equip.size()));
        Check(Pump(false) == 1 && g_LocalReplies.size() == 2,
            "An ignored notification must end the pump with its queue intact instead of freezing the game");
        g_CallbackWake.retryAt = 0;
        Check(Pump() == 2 && g_LocalReplies.empty(), "An ignored notification can retry on a later frame");

        OnSend(g_Coordinator,gc::ProtoFlag|gc::EquipItems,equip.data(),uint32_t(equip.size()));
        gc::SteamCallback callback;
        Check(OnCallback(g_SteamPipe,&callback,nullptr) && callback.callback == 1701 && callback.user == g_SteamUser && callback.size == 4,
            "Local callback uses the correct Steam user and payload ABI");
        callbacks.push_back({123,999,4});
        Check(!OnCallback(g_SteamPipe,&callback,nullptr) && callbacks.size() == 1,
            "A nested dispatch cannot steal the outstanding local event's FreeLastCallback");
        const auto freesBefore = callbackFrees;
        OnFreeCallback(g_SteamPipe);
        Check(callbackFrees == freesBefore && callbacks.size() == 1,
            "Freeing a local event must preserve a real callback that arrived in the meantime");
        Check(OnCallback(999,&callback,nullptr) && callback.callback == 999, "Other Steam pipes pass through unchanged");
        OnFreeCallback(999);
        Check(callbackFrees == freesBefore + 1, "FreeLastCallback on another pipe must reach Steam");
        g_CallbackWake.retryAt = 0; Pump();

        OnSend(g_Coordinator,gc::ProtoFlag|gc::EquipItems,equip.data(),uint32_t(equip.size()));
        g_LocalReplies.front().queuedAt = GetTickCount64() - 2100; TickInventory();
        Check(GetInventoryStatus().pendingReplies == 2 && GetInventoryStatus().oldestReplyMs >= 2100,
            "A stalled queue must report pending replies and their age");
        Pump();
        for (int i = 0; i < 40; ++i)
            OnSend(g_Coordinator,gc::ProtoFlag|gc::EquipItems,equip.data(),uint32_t(equip.size()));
        Check(Pump() == int(gc::CallbackWake::MaxBurst) && !g_LocalReplies.empty(),
            "A rapid-click burst must yield to rendering after a bounded number of local callbacks");
        int recoveryFrames = 0;
        while (!g_LocalReplies.empty()) {
            Check(++recoveryFrames <= 3, "A burst must finish over a bounded number of subsequent frames"); Pump();
        }
        Check(GetInventoryStatus().acceptedEquips == GetInventoryStatus().completedEquips,
            "Yielding a notification burst must not drop any equip or acknowledgement");
        gc::SteamCallback outstanding;
        OnSend(g_Coordinator,gc::ProtoFlag|gc::EquipItems,equip.data(),uint32_t(equip.size()));
        OnCallback(g_SteamPipe,&outstanding,nullptr);
        while (!g_LocalReplies.empty()) { dest.assign(1024,0); OnRetrieve(g_Coordinator,&type,dest.data(),uint32_t(dest.size()),&size); }
        PauseInventory(); TickInventory();
        Check(GetInventoryStatus().hookActive && g_CallbackWake.outstanding,
            "Pause must retain FreeLastCallback until the game's outstanding local event is released");
        OnFreeCallback(g_SteamPipe); TickInventory();
        Check(!GetInventoryStatus().hookActive, "Pause completes after the final callback is released");
        const auto beforeResume = sends;
        Start();
        Check(GetInventoryStatus().equipActive && sends == beforeResume && GetInventoryStatus().recordsDelivered,
            "Resume after pause must preserve the delivered inventory without a full refresh");
        const auto removeEquip = gc::Packet(gc::EquipItems, {10,6,8,0,16,1,24,0});
        OnSend(g_Coordinator,gc::ProtoFlag|gc::EquipItems,equip.data(),uint32_t(equip.size()));
        OnSend(g_Coordinator,gc::ProtoFlag|gc::EquipItems,removeEquip.data(),uint32_t(removeEquip.size()));
        dest.assign(1024,0);
        OnRetrieve(g_Coordinator,&type,dest.data(),uint32_t(dest.size()),&size);
        OnRetrieve(g_Coordinator,&type,dest.data(),uint32_t(dest.size()),&size);
        const auto firstAppearance = appearance::Read();
        Check(firstAppearance->selections[0].item == 7, "The first queued acknowledgement must publish its own outfit, not the newer unequip");
        OnRetrieve(g_Coordinator,&type,dest.data(),uint32_t(dest.size()),&size);
        Check(appearance::Read() == firstAppearance, "The later update alone cannot overtake the first published appearance");
        OnRetrieve(g_Coordinator,&type,dest.data(),uint32_t(dest.size()),&size);
        Check(appearance::Read()->selections[0].item == 0 && appearance::Read()->revision > firstAppearance->revision && firstAppearance->selections[0].item == 7,
            "The later acknowledgement publishes the default while the earlier snapshot remains immutable");
        gc::CallbackWake stale{}; gc::SteamCallback event{};
        Check(stale.Issue(1, 7, 1000, 5, 16, &event) && !stale.Issue(1, 7, 1500, 6, 16, &event),
            "An outstanding notification blocks further wakes while Dota may still hold it");
        Check(stale.Issue(1, 7, 1000 + gc::CallbackWake::StaleAfterMs, 6, 16, &event) && !stale.Release(8) && stale.Release(7),
            "A notification Dota never freed is reclaimed after the deadline instead of stalling replies forever");
        std::cout << "PASS: " << checks << " production receiver checks against a fake GC\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
