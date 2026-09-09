#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

enum class InventoryPhase { Starting, Waiting, Delivered, Paused, TimedOut, Failed };
struct InventoryStatus {
    InventoryPhase phase = InventoryPhase::Starting;
    std::string detail = "Starting inventory receiver";
    size_t items = 0;
    uint64_t polls = 0, receives = 0, packets = 0;
    uint32_t lastType = 0, refreshes = 0;
    bool hookActive = false;
    bool recordsDelivered = false;
    uint64_t cacheDeliveries = 0;
    size_t cachedLocalItems = 0;
    bool equipActive = false;
    uint64_t equipRequests = 0, localEquips = 0, equipUpdates = 0;
    uint64_t acceptedEquips = 0, completedEquips = 0, equipAcks = 0;
    uint64_t callbackPolls = 0, wakeups = 0, oldestReplyMs = 0, lastDeliveryMs = 0;
    uint64_t lastEquipUs = 0, maxEquipUs = 0;
    size_t pendingReplies = 0;
    uint64_t connectionMessages = 0;
    uint32_t connectionStatus = 0;
    uint32_t lastEquipType = 0;
    std::string equipDetail = "No native Equip request received yet";
};

void InitializeInventory();
void TickInventory();
void RequestInventoryRefresh();
void PauseInventory();
InventoryStatus GetInventoryStatus();
