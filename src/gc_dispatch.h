#pragma once
#include <cstddef>
#include <cstdint>

namespace gc {
// Steam's CallbackMsg_t / GCMessageAvailable_t ABI on Win64. The callback is
// consumed by the game's existing Steam dispatcher; we never run it on a worker.
// https://partner.steamgames.com/doc/api/ISteamGameCoordinator#GCMessageAvailable_t
struct SteamCallback {
    int user = 0, callback = 0;
    uint8_t* data = nullptr;
    int size = 0;
};
static_assert(offsetof(SteamCallback, data) == 8 && sizeof(SteamCallback) == 24,
    "Steam callback bridge requires the Win64 ABI");
struct CallbackWake {
    static constexpr uint32_t MaxBurst = 32;
    uint32_t messageSize = 0, thread = 0;
    uint32_t burst = 0;
    static constexpr uint64_t StaleAfterMs = 2000;
    uint64_t lastHead = 0, retryAt = 0, issuedAt = 0;
    bool outstanding = false;
    bool Issue(int user, uint32_t currentThread, uint64_t now, uint64_t head,
               uint32_t size, SteamCallback* callback) {
        // If Dota ignores a notification, return false so its dispatch loop can
        // finish. A retry is allowed on a later frame, not in a tight loop.
        if (!callback) return false;
        // Dota frees our notification on the same pump. If it never does
        // (dropped callback, freed from another thread), reclaim it after a
        // deadline instead of stalling every later reply for the whole session.
        if (outstanding && now - issuedAt < StaleAfterMs) return false;
        outstanding = false;
        if (!head || burst >= MaxBurst || (head == lastHead && now < retryAt)) {
            burst = 0; return false;
        }
        ++burst; // Even a growing/consumed queue must yield back to the game's frame loop.
        messageSize = size; thread = currentThread; outstanding = true;
        lastHead = head; retryAt = now + 16; issuedAt = now;
        *callback = {user, 1701, reinterpret_cast<uint8_t*>(&messageSize), sizeof(messageSize)};
        return true;
    }
    bool Release(uint32_t currentThread) {
        if (!outstanding || thread != currentThread) return false;
        outstanding = false; return true;
    }
};
}
