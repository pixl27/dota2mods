#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace appearance {
struct Selection {
    uint32_t hero = 0, slot = 0;
    uint64_t item = 0;
    uint32_t definition = 0, style = 0;
};
struct Snapshot {
    uint64_t revision = 0, steamId = 0;
    std::vector<Selection> selections;
};
// Published only after Dota consumes the equip reply. Rendering never takes
// the GC receive lock and never follows a mutable inventory container.
inline std::shared_ptr<const Snapshot> current;
inline void Publish(std::shared_ptr<const Snapshot> value) {
    std::atomic_store(&current, std::move(value));
}
inline std::shared_ptr<const Snapshot> Read() { return std::atomic_load(&current); }

struct RenderSlot { uint64_t item = 0; uint32_t definition = 0, style = 0; };
static_assert(sizeof(RenderSlot) == 16, "Native wearable appearance slot ABI");
inline size_t HeroSelections(const Snapshot& snapshot, uint32_t hero) {
    size_t count = 0;
    for (const auto& s : snapshot.selections) if (s.hero == hero && s.slot < 32) ++count;
    return count;
}
inline size_t MatchingSelections(const Snapshot& snapshot, uint32_t hero, const RenderSlot* slots) {
    size_t count = 0;
    for (const auto& s : snapshot.selections) {
        if (s.hero != hero || s.slot >= 32) continue;
        const auto& actual = slots[s.slot];
        // Default selections are resolved by Dota's own item schema.
        if ((!s.item && !actual.item) || (s.item && actual.definition == s.definition && actual.style == s.style)) ++count;
    }
    return count;
}
inline uint64_t HeroFingerprint(const Snapshot& snapshot, uint32_t hero) {
    uint64_t hash = 14695981039346656037ULL;
    for (const auto& s : snapshot.selections) if (s.hero == hero && s.slot < 32) {
        for (auto value : {uint64_t(s.slot), s.item, uint64_t(s.definition), uint64_t(s.style)}) {
            hash ^= value; hash *= 1099511628211ULL;
        }
    }
    return hash;
}
// Game-thread scheduler: changing another hero, repeated acknowledgements and
// rapid clicks must not rebuild the current model every frame.
class Scheduler {
    uint64_t entity_ = 0, fingerprint_ = 0, due_ = 0, healthySince_ = 0, retryAfter_ = 0;
    bool complete_ = false;
    unsigned attempts_ = 0, resyncs_ = 0;
public:
    static constexpr unsigned MaxAttempts = 4, MaxResyncs = 3;
    static constexpr uint64_t HealthyMs = 3000, RecoveryCooldownMs = 30000;
    void Reset() { *this = Scheduler{}; }
    bool Observe(uint64_t entity, uint64_t fingerprint, uint64_t now) {
        if (entity_ == entity && fingerprint_ == fingerprint) return false;
        entity_ = entity; fingerprint_ = fingerprint; due_ = now + 75;
        complete_ = false; attempts_ = 0; resyncs_ = 0;
        healthySince_ = retryAfter_ = 0; return true;
    }
    // The server owns the networked model: the end of a transformation or a
    // full entity update can replace the committed outfit. Use bounded bursts
    // per failure episode. A healthy outfit restores the budget;
    // persistent failures get a cooldown, never a permanent lifetime lockout.
    bool Retry(uint64_t now) {
        healthySince_ = 0;
        if ((!complete_ && !Exhausted()) || now < retryAfter_) return false;
        if (resyncs_ >= MaxResyncs) resyncs_ = 0;
        retryAfter_ = resyncs_ + 1 == MaxResyncs ? now + RecoveryCooldownMs : 0;
        ++resyncs_; complete_ = false; attempts_ = 0; due_ = now + 75; return true;
    }
    void Healthy(uint64_t now) {
        if (!complete_) return;
        if (!healthySince_) healthySince_ = now;
        if (now - healthySince_ >= HealthyMs) { resyncs_ = 0; retryAfter_ = 0; }
    }
    void Unhealthy() { healthySince_ = 0; }
    unsigned Resyncs() const { return resyncs_; }
    bool Begin(uint64_t now) {
        if (!entity_ || complete_ || attempts_ >= MaxAttempts || now < due_) return false;
        ++attempts_; due_ = now + 250; return true;
    }
    void Complete() { complete_ = true; }
    bool CompleteState() const { return complete_; }
    bool Exhausted() const { return !complete_ && attempts_ >= MaxAttempts; }
    unsigned Attempts() const { return attempts_; }
};
}
