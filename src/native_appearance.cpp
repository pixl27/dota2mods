#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include "appearance_state.h"
#include "native_appearance.h"
#include "native_appearance_profile.h"
#include "native_appearance_resolver.h"
#include "diagnostics.h"
#include "../thirdparty/minhook/include/MinHook.h"
#pragma comment(lib, "bcrypt.lib")

namespace appearance {
namespace {
struct NativeVector { int32_t count, padding; void** data; int32_t capacity, flags; };
static_assert(sizeof(NativeVector) == 24, "Current Dota item-view vector ABI");
struct CreationEntry { void* view; uint32_t definition; uint8_t style, owned, model, padding; };
static_assert(sizeof(CreationEntry) == 16);
struct Engine {
    void* (__fastcall* allocate)(size_t) = nullptr;
    void (__fastcall* free)(void*) = nullptr;
    void* (__fastcall* kvCtor)(void*, void*, uint8_t) = nullptr;
    void (__fastcall* kvDtor)(void*) = nullptr;
    void (__fastcall* prepare)(void*, void*, void*) = nullptr;
    void (__fastcall* create)(void*, void*) = nullptr;
    void (__fastcall* destroy)(void*) = nullptr;
    void* (__fastcall* modifierCtor)(void*) = nullptr;
    void* (__fastcall* modifierManager)() = nullptr;
    void (__fastcall* populate)(void*, void*, int32_t, void**) = nullptr;
    void* (__fastcall* modelOverride)(void*, void*) = nullptr;
    void (__fastcall* setModel)(void*, const char*) = nullptr;
    void* (__fastcall* modelHandle)(void*, void**) = nullptr;
    const char* (__fastcall* kvModel)(void*, const char*) = nullptr;
    void (__fastcall* setBaseModel)(void*, void*) = nullptr;
    bool (__fastcall* modelReady)(void*) = nullptr;
    const char* (__fastcall* viewModel)(void*, int32_t) = nullptr;
    void (__fastcall* nameCtor)(void*, const char*) = nullptr;
    bool (__fastcall* nameIsType)(void*, uint32_t) = nullptr;
    void (__fastcall* namePurge)(void*, int32_t) = nullptr;
    void* (__fastcall* entityModels)(void*) = nullptr;
} engine;
struct PointerList { int32_t count, padding; void** data; };
// CResourceNameTyped: a 0xc8-byte inline CBufferString followed by the path
// and type hashes. Callers zero it before the engine fills the hashes.
struct alignas(8) ResourceName { int32_t length = 0; uint32_t allocated = 0xc00000c8; char storage[0xc8]{}; uint64_t hash = 0, type = 0; };
static_assert(sizeof(ResourceName) == 0xe0, "Current CResourceNameTyped ABI");
std::array<char, 264> baselineModel{};
std::array<std::string, 1000> heroBaselines;
uint64_t baselineEntity = 0;
struct StagedOutfit {
    NativeVector list{};
    void* modifiers = nullptr;
    std::vector<std::string> paths;
    std::vector<void*> models;
    std::string sourceModel;
    uint64_t started = 0;
} staged;
// After a commit, read back which model the entity actually renders, then
// keep checking it: the server owns the networked model, so the end of a
// transformation or a full entity update can put the classic model back.
uint64_t verifyAt = 0, checkAt = 0;
constexpr uint64_t VerifyDelayMs = 2000, CheckIntervalMs = 500;
std::vector<std::string> committedPaths;
std::vector<void*> committedModels;
std::string committedBase;
uint64_t lastLocalThink = 0, trackedEntity = 0, spawnEntity = 0, spawnGeneration = 0, seenSpawnGeneration = 0;
void* trackedHero = nullptr;
constexpr uint64_t SessionGapMs = 5000;
bool ModelMatches(const char* name, const std::string& path) {
    if (!name || !*name || path.empty()) return false;
    if (path == name) return true;
    auto stem = path;
    if (stem.size() >= 5 && stem.compare(stem.size() - 5, 5, ".vmdl") == 0) stem.resize(stem.size() - 5);
    const std::string prefix = stem + "_c_";
    return !strncmp(name, prefix.c_str(), prefix.size());
}
bool ErrorModel(const char* name) { return name && ModelMatches(name, "models/dev/error.vmdl"); }
void RememberCommitted(const std::vector<std::string>& paths) {
    committedPaths = paths;
    committedBase = paths.empty() ? "" : paths.front();
}
// Combined meshes are named after their base ("<base>_c_<n>.vmdl") and a
// transformation switches to another replacement of the same outfit; both
// count as ours. Anything else, including models/dev/error.vmdl, does not.
bool RenderMatchesCommitted(const char* name) {
    if (!*name || committedPaths.empty()) return false;
    for (const auto& path : committedPaths) if (ModelMatches(name, path)) return true;
    return false;
}
// Unfamiliar models can be hex, shapeshift or another legitimate ability.
// Only the captured base, the selected base and an error model authorize a
// reconstruction. In particular a dragon must never be rebuilt as a human.
bool TemporaryForm(const char* name) {
    return *name && !ErrorModel(name) && baselineModel[0] &&
        !ModelMatches(name, baselineModel.data()) && !ModelMatches(name, committedBase);
}
using ThinkFn = void(__fastcall*)(void*);
using PlayerInventoryFn = void*(__fastcall*)(void*, int32_t, bool);
using EquippedFn = void*(__fastcall*)(void*, uint32_t, uint32_t, bool);
using FindItemFn = void*(__fastcall*)(void*, uint64_t, int32_t*);
using DefaultFn = void*(__fastcall*)(void*, uint32_t, uint32_t);
using BuildListFn = void(__fastcall*)(void*, void*, void*, NativeVector*);
using WearablePlayerFn = int32_t*(__fastcall*)(void*, int32_t*);
uintptr_t base = 0;
// tier0 does not export the allocator by name; client.dll reaches it through an
// import slot, and both of its allocation thunks jump straight into the interface.
void* MemoryInterface() {
    const auto imported = *reinterpret_cast<void**>(base + profile::MemAllocImport);
    return imported ? *reinterpret_cast<void**>(imported) : nullptr;
}
void* __fastcall EngineAllocate(size_t bytes) {
    const auto memory = MemoryInterface();
    if (!memory) return nullptr;
    return reinterpret_cast<void*(__fastcall*)(void*, size_t)>((*static_cast<void***>(memory))[profile::MemAllocSlot])(memory, bytes);
}
void __fastcall EngineFree(void* block) {
    const auto memory = MemoryInterface();
    if (memory) reinterpret_cast<void(__fastcall*)(void*, void*)>((*static_cast<void***>(memory))[profile::MemFreeSlot])(memory, block);
}
uintptr_t LocalInventory() { return base + profile::InventoryManager + profile::LocalInventoryOffset; }
ThinkFn originalThink = nullptr;
PlayerInventoryFn originalPlayerInventory = nullptr;
EquippedFn originalEquipped = nullptr;
FindItemFn findItem = nullptr;
DefaultFn defaultView = nullptr;
BuildListFn originalBuildList = nullptr, originalSpawnList = nullptr;
WearablePlayerFn wearablePlayer = nullptr;
DWORD lookupTls = TLS_OUT_OF_INDEXES;
struct ListContext {
    const Snapshot* snapshot;
    void* hero;
    uint32_t heroId;
    NativeVector* output;
};
ListContext* ActiveList() {
    return lookupTls == TLS_OUT_OF_INDEXES ? nullptr : static_cast<ListContext*>(TlsGetValue(lookupTls));
}
struct ScopedList {
    ListContext* previous;
    explicit ScopedList(ListContext& value) : previous(ActiveList()) { TlsSetValue(lookupTls, &value); }
    ~ScopedList() { TlsSetValue(lookupTls, previous); }
};
std::mutex statusMutex;
Status status;
// These fields are accessed exclusively on the local hero's ClientThink thread.
Scheduler scheduler;

void SetPhase(Phase phase) { std::lock_guard<std::mutex> lock(statusMutex); status.phase = phase; }

bool DiskMatches(HMODULE module) {
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(module, path, DWORD(std::size(path)));
    if (!length || length >= std::size(path)) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::array<UCHAR, 65536> buffer{}; DWORD read = 0;
    while (ok) {
        if (!ReadFile(file, buffer.data(), DWORD(buffer.size()), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    UCHAR digest[32]{};
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    char hex[65]{};
    for (size_t i = 0; i < sizeof(digest); ++i) sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
    return ok && std::strcmp(hex, profile::Sha256) == 0;
}

bool LocalHero(void* hero, uint64_t steamId, uint32_t& heroId, uint32_t& handle) {
    __try {
        const auto controller = *reinterpret_cast<uintptr_t*>(base + profile::LocalController);
        if (!controller || !steamId || *reinterpret_cast<uint64_t*>(controller + profile::SteamId) != steamId) return false;
        handle = *reinterpret_cast<uint32_t*>(controller + profile::AssignedHero);
        if (handle >= 0xfffffffe) return false;
        const auto chunks = *reinterpret_cast<uintptr_t*>(base + profile::EntityChunks);
        if (!chunks) return false;
        const auto index = handle & 0x7fff;
        const auto chunk = *reinterpret_cast<uintptr_t*>(chunks + (index >> 9) * sizeof(uintptr_t));
        if (!chunk) return false;
        const auto identity = chunk + (index & 0x1ff) * profile::IdentityStride;
        if (*reinterpret_cast<uint32_t*>(identity + profile::IdentityHandle) != handle || *reinterpret_cast<void**>(identity) != hero) return false;
        heroId = *reinterpret_cast<uint32_t*>(uintptr_t(hero) + profile::HeroId);
        return heroId > 0 && heroId < 1000;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* __fastcall OnPlayerInventory(void* manager, int32_t player, bool local) {
    // In a match, the player-resource inventory can differ from the local
    // menu inventory. Redirect only inside our local hero appearance gather.
    if (ActiveList()) return reinterpret_cast<void*>(LocalInventory());
    return originalPlayerInventory(manager, player, local);
}
void* __fastcall OnEquipped(void* inventory, uint32_t hero, uint32_t slot, bool ignorePreview) {
    const auto context = ActiveList();
    const auto snapshot = context ? context->snapshot : nullptr;
    const auto wantedHero = context ? context->heroId : 0;
    if (snapshot && hero == wantedHero && slot < 32) {
        for (const auto& selection : snapshot->selections) if (selection.hero == hero && selection.slot == slot) {
            if (!selection.item) return defaultView(reinterpret_cast<void*>(base + profile::InventoryManager), hero, slot);
            // Resolve the acknowledged 64-bit ID directly. A stale per-hero
            // loadout index must not return the preceding click's item view.
            return findItem(reinterpret_cast<void*>(LocalInventory()), selection.item, nullptr);
        }
    }
    return originalEquipped(inventory, hero, slot, ignorePreview);
}

bool LocalWearableOwner(void* hero, const Snapshot& snapshot, uint32_t& heroId) {
    // During native spawning the assigned-hero handle may not exist yet.
    // Dota's own wearable-owner resolver must match our signed-in controller.
    __try {
        const auto controller = *reinterpret_cast<uintptr_t*>(base + profile::LocalController);
        if (!controller || !snapshot.steamId || *reinterpret_cast<uint64_t*>(controller + profile::SteamId) != snapshot.steamId) return false;
        int32_t owner = -1;
        wearablePlayer(hero, &owner);
        if (owner < 0 || owner >= 64 || owner != *reinterpret_cast<int32_t*>(controller + profile::PlayerId)) return false;
        heroId = *reinterpret_cast<uint32_t*>(uintptr_t(hero) + profile::HeroId);
        return heroId > 0 && heroId < 1000;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CaptureBaseline(void* hero, uint64_t entity);
uint64_t EntityKey(void* hero, uint32_t handle) {
    return uint64_t(uintptr_t(hero)) ^ (uint64_t(handle) << 32);
}
void CaptureSpawnModel(void* hero, void* kv) {
    const auto identity = *reinterpret_cast<uintptr_t*>(uintptr_t(hero) + profile::EntityIdentity);
    if (!identity || *reinterpret_cast<void**>(identity) != hero) return;
    const auto entity = EntityKey(hero, *reinterpret_cast<uint32_t*>(identity + profile::IdentityHandle));
    const auto heroId = *reinterpret_cast<uint32_t*>(uintptr_t(hero) + profile::HeroId);
    if (!heroId || heroId >= heroBaselines.size()) return;
    // Capture before inventory modifiers can replace the render/base model.
    // Empty KVs from a live update must never overwrite this spawn baseline.
    const char* model = kv && engine.kvModel ? engine.kvModel(kv, nullptr) : nullptr;
    if (model && *model && !ErrorModel(model) &&
        (heroBaselines[heroId].empty() || ModelMatches(model, heroBaselines[heroId]))) {
        strncpy_s(baselineModel.data(), baselineModel.size(), model, _TRUNCATE);
        baselineEntity = entity;
        heroBaselines[heroId] = model;
    } else if (engine.modelHandle) CaptureBaseline(hero, entity);
}
void __fastcall OnSpawnList(void* hero, void* kv, void* resources, NativeVector* output) {
    const auto context = ActiveList();
    // Preserve the engine's empty-list fallback into inventory selection.
    // This skips only the stale spawn-KV list for this exact native call.
    if (context && context->hero == hero && context->output == output && output && output->count == 0) return;
    originalSpawnList(hero, kv, resources, output);
}
void __fastcall OnBuildList(void* hero, void* kv, void* resources, NativeVector* output) {
    const auto published = Read();
    const auto parent = ActiveList();
    const auto snapshot = parent && parent->hero == hero ? parent->snapshot : published.get();
    uint32_t heroId = 0;
    if (!snapshot || !output || output->count || (parent && parent->output) || !LocalWearableOwner(hero, *snapshot, heroId) || !HeroSelections(*snapshot, heroId)) {
        originalBuildList(hero, kv, resources, output); return;
    }
    // Resolve everything before replacing the spawn source. Missing GC data
    // leaves Dota's original creation path intact.
    for (const auto& selection : snapshot->selections) if (selection.hero == heroId && selection.slot < 32 && selection.item) {
        if (!findItem(reinterpret_cast<void*>(LocalInventory()), selection.item, nullptr)) {
            originalBuildList(hero, kv, resources, output); return;
        }
    }
    if (!parent) {
        CaptureSpawnModel(hero, kv);
        const auto identity = *reinterpret_cast<uintptr_t*>(uintptr_t(hero) + profile::EntityIdentity);
        if (identity && *reinterpret_cast<void**>(identity) == hero) {
            spawnEntity = EntityKey(hero, *reinterpret_cast<uint32_t*>(identity + profile::IdentityHandle));
            ++spawnGeneration;
        }
    }
    ListContext context{snapshot, hero, heroId, output};
    ScopedList scope(context);
    originalBuildList(hero, kv, resources, output);
    std::lock_guard<std::mutex> lock(statusMutex);
    ++status.wearableLists;
}

void ReleaseCreationList(NativeVector& list) {
    auto entries = reinterpret_cast<CreationEntry*>(list.data);
    for (int32_t i = 0; i < list.count; ++i) if (entries[i].owned && entries[i].view) {
        auto destructor = reinterpret_cast<void(__fastcall*)(void*, uint32_t)>((*static_cast<void***>(entries[i].view))[profile::ViewDestructorSlot]);
        destructor(entries[i].view, 1);
    }
    if (list.data && !(uint32_t(list.flags) & 0xc0000000)) engine.free(list.data);
    list = {};
}
void ReleaseModifiers(void* modifiers) {
    if (modifiers && InterlockedDecrement(reinterpret_cast<LONG*>(uintptr_t(modifiers) + profile::ModifierRefCount)) == 0) {
        auto destructor = reinterpret_cast<void(__fastcall*)(void*, uint32_t)>((*static_cast<void***>(modifiers))[profile::ModifierDestructorSlot]);
        destructor(modifiers, 1);
    }
}
bool CaptureBaseline(void* hero, uint64_t entity) {
    if (entity == baselineEntity && baselineModel[0]) return true;
    const auto heroId = *reinterpret_cast<uint32_t*>(uintptr_t(hero) + profile::HeroId);
    if (!heroId || heroId >= heroBaselines.size()) return false;
    baselineModel.fill(0);
    // Entity handles change on reconnect. Preserve the known original model
    // for each hero instead of learning an error/persona as the new baseline.
    if (!heroBaselines[heroId].empty()) {
        strncpy_s(baselineModel.data(), baselineModel.size(), heroBaselines[heroId].c_str(), _TRUNCATE);
        baselineEntity = entity;
        return true;
    }
    void* handle = *reinterpret_cast<void**>(uintptr_t(hero) + profile::BaseModel);
    if (!handle) engine.modelHandle(hero, &handle);
    const auto info = *reinterpret_cast<void**>(base + profile::ModelInfo);
    if (!info || !handle) return false;
    auto name = reinterpret_cast<void(__fastcall*)(void*, void*, char*, uint32_t)>((*static_cast<void***>(info))[profile::ModelNameSlot]);
    name(info, handle, baselineModel.data(), uint32_t(baselineModel.size()));
    baselineModel.back() = 0;
    baselineEntity = entity;
    if (ErrorModel(baselineModel.data())) baselineModel.fill(0);
    if (baselineModel[0]) heroBaselines[heroId] = baselineModel.data();
    return baselineModel[0] != 0;
}
void ReleaseModels(std::vector<void*>& models) {
    const auto resources = *reinterpret_cast<void**>(base + profile::ResourceSystem);
    for (auto model : models) if (model && resources &&
        InterlockedDecrement(reinterpret_cast<LONG*>(uintptr_t(model) + profile::ModelRefCount)) == 0) {
        auto release = reinterpret_cast<void(__fastcall*)(void*, void*)>((*static_cast<void***>(resources))[profile::ReleaseSlot]);
        release(resources, model);
    }
    models.clear();
}
void ReleaseStaged() {
    ReleaseCreationList(staged.list);
    ReleaseModifiers(staged.modifiers);
    ReleaseModels(staged.models);
    staged = {};
}
bool StageWearables(void* hero, const Snapshot& snapshot, uint32_t heroId, uint64_t now) {
    for (const auto& s : snapshot.selections) if (s.hero == heroId && s.slot < 32 && s.item &&
        !findItem(reinterpret_cast<void*>(LocalInventory()), s.item, nullptr)) return false;
    auto& list = *reinterpret_cast<NativeVector*>(uintptr_t(hero) + profile::CreationList);
    auto& initialized = *reinterpret_cast<uint8_t*>(uintptr_t(hero) + profile::CreationInitialized);
    if (list.count < 0 || list.count > 64 || (list.count && !list.data) || !baselineModel[0]) return false;
    ListContext context{&snapshot, hero, heroId, nullptr};
    ScopedList scope(context);

    // Build an engine-owned replacement before releasing the visible outfit.
    // CEntityKeyValues is 0x38 bytes in the verified constructor/destructor.
    alignas(8) std::array<uint8_t, profile::KeyValuesStorage> kv{}, resources{};
    engine.kvCtor(kv.data(), nullptr, 2); engine.kvCtor(resources.data(), nullptr, 2);
    NativeVector previous = list;
    const auto wasInitialized = initialized;
    list = {}; initialized = 0;
    engine.prepare(hero, kv.data(), resources.data());
    staged.list = list; list = previous; initialized = wasInitialized;
    engine.kvDtor(resources.data()); engine.kvDtor(kv.data());
    if (staged.list.count <= 0 || staged.list.count > 64 || !staged.list.data) {
        ReleaseStaged(); return false;
    }
    std::array<void*, 64> views{}; int32_t count = 0;
    auto entries = reinterpret_cast<const CreationEntry*>(staged.list.data);
    for (int32_t i = 0; i < staged.list.count; ++i) if (entries[i].view) views[count++] = entries[i].view;
    auto modifiers = engine.allocate(profile::ModifierStorage);
    if (!modifiers || !count) {
        if (modifiers) engine.free(modifiers);
        ReleaseStaged(); return false;
    }
    modifiers = engine.modifierCtor(modifiers);
    engine.populate(engine.modifierManager(), modifiers, count, views.data());
    auto& activeModifiers = *reinterpret_cast<void**>(uintptr_t(hero) + profile::Modifiers);
    auto previousModifiers = activeModifiers;
    activeModifiers = modifiers;
    // Model replacement and persona skeleton are attributes of the whole
    // outfit. Combining meshes alone cannot apply them.
    auto replacement = engine.modelOverride(hero, nullptr);
    const char* model = replacement ? *reinterpret_cast<const char**>(uintptr_t(replacement) + profile::ModifierPath) : nullptr;
    if (!model || !*model) model = baselineModel.data();
    staged.paths.emplace_back(model);
    activeModifiers = previousModifiers;
    staged.modifiers = modifiers;
    const auto modelIndex = *reinterpret_cast<int32_t*>(uintptr_t(hero) + profile::ModelIndex);
    for (int32_t i = 0; i < count; ++i) {
        const auto path = engine.viewModel(views[i], modelIndex);
        if (path && *path) staged.paths.emplace_back(path);
    }
    // Transformations (Elder Dragon Form, Shadow Wolves...) switch to the
    // outfit's other entity_model replacements through SetModel(path) too.
    // Register and load them now so the swap cannot resolve to the error model.
    if (engine.entityModels) {
        if (const auto replacements = static_cast<const PointerList*>(engine.entityModels(modifiers))) {
            for (int32_t i = 0; i < replacements->count && i < 64 && replacements->data; ++i) {
                const auto entry = static_cast<const uint8_t*>(replacements->data[i]);
                const auto path = entry ? *reinterpret_cast<const char* const*>(entry + profile::ModifierPath) : nullptr;
                if (path && *path && std::find(staged.paths.begin(), staged.paths.end(), path) == staged.paths.end())
                    staged.paths.emplace_back(path);
            }
        }
    }
    staged.started = now;
    { std::lock_guard<std::mutex> lock(statusMutex);
      status.prepareMs = uint32_t(GetTickCount64() - now);
      strcpy_s(status.baseModel, baselineModel.data());
      strncpy_s(status.selectedModel, staged.paths.front().c_str(), _TRUNCATE); }
    return true;
}
// FindOrLoadModel only creates an empty handle for a model no server precached
// ("requested but is not in the system"), which renders as models/dev/error.vmdl.
// Dota's model combiner registers such models in its just-in-time manifest
// first; a persona base model is never precached by a server that does not know
// about the local outfit, so do the same before resolving any handle.
enum class Registration { Unavailable, NotModel, Known, Registered };
Registration RegisterModel(const char* path) {
    const auto resources = *reinterpret_cast<void**>(base + profile::ResourceSystemInterface);
    if (!resources || !engine.nameCtor || !engine.nameIsType || !engine.namePurge) return Registration::Unavailable;
    ResourceName name{};
    engine.nameCtor(&name, path);
    auto result = Registration::NotModel;
    if ((uint32_t(name.length) & 0x3fffffff) && engine.nameIsType(&name, profile::ModelResourceType)) {
        const auto table = *static_cast<void***>(resources);
        const auto state = reinterpret_cast<int32_t(__fastcall*)(void*, ResourceName*)>(table[profile::ResourceStateSlot])(resources, &name);
        if (state) {
            reinterpret_cast<void*(__fastcall*)(void*, ResourceName*, bool)>(table[profile::ResourceFindSlot])(resources, &name, false);
            result = Registration::Known;
        } else {
            reinterpret_cast<void*(__fastcall*)(void*, ResourceName*, const char*)>(table[profile::ResourceRegisterSlot])(
                resources, &name, reinterpret_cast<const char*>(base + profile::JustInTimeManifest));
            result = Registration::Registered;
        }
    }
    engine.namePurge(&name, 0);
    return result;
}
bool ModelsReady() {
    const auto info = *reinterpret_cast<void**>(base + profile::ModelInfo);
    if (!info) return false;
    auto load = reinterpret_cast<void*(__fastcall*)(void*, void**, const char*)>((*static_cast<void***>(info))[profile::FindOrLoadSlot]);
    // Spread resource requests over frames; preserve the complete old outfit
    // while Dota loads the replacement. Never wait/sleep on its render thread.
    for (unsigned budget = 0; budget < 2 && staged.models.size() < staged.paths.size(); ++budget) {
        void* model = nullptr;
        const auto& path = staged.paths[staged.models.size()];
        const auto registration = RegisterModel(path.c_str());
        { std::lock_guard<std::mutex> lock(statusMutex);
          if (registration == Registration::Registered) ++status.registered;
          else if (registration == Registration::Known) ++status.known;
          else if (registration == Registration::Unavailable) ++status.unavailable; }
        load(info, &model, path.c_str());
        if (model) InterlockedIncrement(reinterpret_cast<LONG*>(uintptr_t(model) + profile::ModelRefCount));
        staged.models.push_back(model);
    }
    if (staged.models.size() != staged.paths.size()) return false;
    for (auto model : staged.models) {
        if (!model || !engine.modelReady(model)) return false;
        char name[264]{};
        reinterpret_cast<void(__fastcall*)(void*, void*, char*, uint32_t)>((*static_cast<void***>(info))[profile::ModelNameSlot])(
            info, model, name, uint32_t(sizeof(name)));
        name[sizeof(name) - 1] = 0;
        if (!*name || ErrorModel(name)) return false;
    }
    return true;
}
bool CommitWearables(void* hero, const Snapshot& snapshot, uint32_t heroId) {
    ListContext context{&snapshot, hero, heroId, nullptr};
    ScopedList scope(context);
    auto& list = *reinterpret_cast<NativeVector*>(uintptr_t(hero) + profile::CreationList);
    auto& activeModifiers = *reinterpret_cast<void**>(uintptr_t(hero) + profile::Modifiers);
    NativeVector previous = list;
    auto previousModifiers = activeModifiers;
    list = staged.list; staged.list = {};
    *reinterpret_cast<uint8_t*>(uintptr_t(hero) + profile::CreationInitialized) = 1;
    activeModifiers = staged.modifiers; staged.modifiers = nullptr;
    alignas(8) std::array<uint8_t, profile::KeyValuesStorage> kv{};
    engine.kvCtor(kv.data(), nullptr, 2);
    engine.destroy(hero);
    // The combiner keeps its own reference. Reset it too when leaving a
    // persona, otherwise it keeps combining classic items on the old skeleton.
    engine.setBaseModel(hero, staged.models.front());
    engine.setModel(hero, staged.paths.front().c_str());
    engine.create(hero, kv.data());
    ReleaseModifiers(previousModifiers);
    ReleaseCreationList(previous);
    engine.kvDtor(kv.data());
    // Let the native combined-model path gather the newly created, persona-
    // filtered wearable entities, rather than all inventory slots together.
    *reinterpret_cast<uint8_t*>(uintptr_t(hero) + profile::DirtyFlags) |= 12;
    return *reinterpret_cast<int32_t*>(uintptr_t(hero) + profile::WearableCount) > 0;
}

void RecordRenderModel(void* hero, char (&name)[264]) {
    // The HUD reports only native acceptance; this reads what Dota draws.
    const auto info = *reinterpret_cast<void**>(base + profile::ModelInfo);
    void* handle = nullptr;
    if (engine.modelHandle) engine.modelHandle(hero, &handle);
    name[0] = 0;
    if (info && handle) {
        auto text = reinterpret_cast<void(__fastcall*)(void*, void*, char*, uint32_t)>((*static_cast<void***>(info))[profile::ModelNameSlot]);
        text(info, handle, name, uint32_t(sizeof(name))); name[sizeof(name) - 1] = 0;
    }
    std::lock_guard<std::mutex> lock(statusMutex);
    strcpy_s(status.renderModel, name);
}
void __fastcall OnThink(void* hero) {
    originalThink(hero);
    const auto snapshot = Read();
    uint32_t heroId = 0, handle = 0;
    if (!snapshot || !LocalHero(hero, snapshot->steamId, heroId, handle) || !HeroSelections(*snapshot, heroId)) {
        if (hero == trackedHero) {
            ReleaseStaged(); scheduler.Reset(); lastLocalThink = 0;
            verifyAt = checkAt = 0;
        }
        return;
    }
    const auto now = GetTickCount64();
    const uint64_t entity = EntityKey(hero, handle);
    const bool newEntity = entity != trackedEntity;
    const bool resumed = !lastLocalThink || now - lastLocalThink > SessionGapMs;
    const bool respawned = spawnEntity == entity && seenSpawnGeneration != spawnGeneration;
    if (newEntity || resumed || respawned) {
        ReleaseStaged(); scheduler.Reset(); verifyAt = checkAt = 0;
        // Re-resolve every resource after an entity/session restart. Do not
        // reuse a completed stage or assume the old manifest still exists.
        ReleaseModels(committedModels);
        if (newEntity) { committedPaths.clear(); committedBase.clear(); }
    }
    trackedHero = hero; trackedEntity = entity; lastLocalThink = now;
    seenSpawnGeneration = spawnGeneration;
    const bool changed = scheduler.Observe(entity, HeroFingerprint(*snapshot, heroId), now);
    if (changed) {
        ReleaseStaged(); verifyAt = 0; checkAt = now + CheckIntervalMs;
        std::lock_guard<std::mutex> lock(statusMutex);
        status.phase = Phase::Pending; status.hero = heroId; status.revision = snapshot->revision;
        status.expected = uint32_t(HeroSelections(*snapshot, heroId)); status.matched = 0; status.attempts = 0;
    }
    char currentName[264]{};
    // Re-read while preparing: an ability may change form while resources load.
    if (!scheduler.CompleteState() || (checkAt && now >= checkAt) || (verifyAt && now >= verifyAt))
        RecordRenderModel(hero, currentName);
    if (staged.list.data && staged.sourceModel != currentName) ReleaseStaged();
    const bool transformed = !committedBase.empty() && TemporaryForm(currentName);
    if (!staged.list.data && !transformed && scheduler.Begin(now)) {
        if (CaptureBaseline(hero, entity) && StageWearables(hero, *snapshot, heroId, now))
            staged.sourceModel = currentName;
        std::lock_guard<std::mutex> lock(statusMutex); status.attempts = scheduler.Attempts();
    }
    if (staged.list.data && (!TemporaryForm(currentName) || ModelMatches(currentName, staged.paths.front())) && ModelsReady()) {
        const auto begin = GetTickCount64();
        const bool rebuilt = CommitWearables(hero, *snapshot, heroId);
        const auto elapsed = GetTickCount64() - begin;
        RememberCommitted(staged.paths);
        ReleaseModels(committedModels);
        committedModels = std::move(staged.models);
        ReleaseStaged(); scheduler.Complete(); verifyAt = now + VerifyDelayMs; checkAt = 0;
        std::lock_guard<std::mutex> lock(statusMutex);
        ++status.rebuilds; status.commitMs = uint32_t(elapsed);
        status.phase = rebuilt ? Phase::Prepared : Phase::MissingItems;
        status.matched = rebuilt ? status.expected : 0;
        status.views = rebuilt ? uint32_t(reinterpret_cast<NativeVector*>(uintptr_t(hero) + profile::CreationList)->count) : 0;
    } else if (staged.list.data && now - staged.started > 10000) {
        ReleaseStaged(); scheduler.Complete(); checkAt = now + CheckIntervalMs; SetPhase(Phase::MissingItems);
    }
    if (verifyAt && now >= verifyAt) {
        char name[264]; verifyAt = 0; checkAt = now + CheckIntervalMs; RecordRenderModel(hero, name);
    } else if (checkAt && now >= checkAt && !staged.list.data && (scheduler.CompleteState() || scheduler.Exhausted())) {
        char name[264]; checkAt = now + CheckIntervalMs; RecordRenderModel(hero, name);
        const bool wearablesMissing = *reinterpret_cast<int32_t*>(uintptr_t(hero) + profile::WearableCount) <= 0;
        const bool failed = NativeStatus().phase == Phase::MissingItems || scheduler.Exhausted();
        const bool mismatch = *name && !TemporaryForm(name) &&
            (failed || !RenderMatchesCommitted(name) || wearablesMissing);
        if (mismatch && scheduler.Retry(now)) {
            std::lock_guard<std::mutex> lock(statusMutex); ++status.resyncs; status.phase = Phase::Pending; status.matched = 0;
        } else if (!failed && RenderMatchesCommitted(name) && !TemporaryForm(name) && !wearablesMissing) scheduler.Healthy(now);
        else scheduler.Unhealthy();
    }
    if (!staged.list.data && scheduler.Exhausted()) SetPhase(Phase::MissingItems);
    // Readable diagnostics stay off the model-loading path; the HUD only
    // reports native input acceptance, not proof of completed visual loading.
    { std::lock_guard<std::mutex> lock(statusMutex); status.lastSeenMs = now; }
}
profile::Resolution ResolveProfile() {
    __try { return profile::Resolver().Run(base); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        profile::Resolution failed;
        strcpy_s(failed.failure, "image: unreadable while resolving");
        return failed;
    }
}
}

void InitializeNative() {
    const auto module = GetModuleHandleW(L"client.dll"); base = uintptr_t(module);
    if (!base) { SetPhase(Phase::Unsupported); return; }
    // Relocate the profile before anything reads an address from it. A Dota
    // update usually only moves this code, which the resolver follows on its
    // own; the disk hash only records whether this is the build we verified.
    const auto resolution = ResolveProfile();
    // Hashing a hundred megabytes is far too slow to do while holding the lock
    // the render thread reads the status under, and it only labels the result.
    const bool exact = resolution.exact && DiskMatches(module);
    { std::lock_guard<std::mutex> lock(statusMutex);
      status.profileExact = exact;
      status.profileMoved = resolution.moved;
      strcpy_s(status.profileDetail, resolution.resolved
          ? (exact ? "build vérifiée" : "relocalisé pour une build mise à jour") : resolution.failure); }
    if (!resolution.resolved) { SetPhase(Phase::Unsupported); return; }
    findItem = reinterpret_cast<FindItemFn>(base + profile::FindItemView);
    defaultView = reinterpret_cast<DefaultFn>(base + profile::DefaultView);
    wearablePlayer = reinterpret_cast<WearablePlayerFn>(base + profile::WearablePlayer);
#define NATIVE_MEMBER(member, offset) engine.member = reinterpret_cast<decltype(engine.member)>(base + profile::offset)
    engine.allocate = &EngineAllocate; engine.free = &EngineFree;
    NATIVE_MEMBER(kvCtor, KeyValuesCtor); NATIVE_MEMBER(kvDtor, KeyValuesDtor);
    NATIVE_MEMBER(prepare, PrepareWearables); NATIVE_MEMBER(create, CreateWearables); NATIVE_MEMBER(destroy, DestroyWearables);
    NATIVE_MEMBER(modifierCtor, ModifierCtor); NATIVE_MEMBER(modifierManager, ModifierManager); NATIVE_MEMBER(populate, PopulateModifiers);
    NATIVE_MEMBER(modelOverride, ModelOverride); NATIVE_MEMBER(setModel, SetModel); NATIVE_MEMBER(modelHandle, ModelHandle);
    NATIVE_MEMBER(kvModel, KeyValuesModel); NATIVE_MEMBER(setBaseModel, SetBaseModel);
    NATIVE_MEMBER(modelReady, ModelReady); NATIVE_MEMBER(viewModel, ViewModel);
    NATIVE_MEMBER(nameCtor, ResourceNameCtor); NATIVE_MEMBER(nameIsType, ResourceNameIsType);
    NATIVE_MEMBER(entityModels, EntityModelModifiers);
#undef NATIVE_MEMBER
    if (const auto tier0 = GetModuleHandleW(L"tier0.dll"))
        engine.namePurge = reinterpret_cast<decltype(engine.namePurge)>(GetProcAddress(tier0, "?Purge@CBufferString@@QEAAXH@Z"));
    lookupTls = TlsAlloc();
    if (lookupTls == TLS_OUT_OF_INDEXES) { SetPhase(Phase::HookFailed); return; }
    struct Hook { uintptr_t offset; void* detour; void** original; };
    const Hook hooks[] = {
        {profile::InventoryForPlayer, reinterpret_cast<void*>(&OnPlayerInventory), reinterpret_cast<void**>(&originalPlayerInventory)},
        {profile::EquippedView, reinterpret_cast<void*>(&OnEquipped), reinterpret_cast<void**>(&originalEquipped)},
        {profile::BuildSpawnWearableList, reinterpret_cast<void*>(&OnSpawnList), reinterpret_cast<void**>(&originalSpawnList)},
        {profile::BuildWearableList, reinterpret_cast<void*>(&OnBuildList), reinterpret_cast<void**>(&originalBuildList)},
        {profile::Think, reinterpret_cast<void*>(&OnThink), reinterpret_cast<void**>(&originalThink)}
    };
    size_t created = 0;
    for (const auto& hook : hooks) {
        if (MH_CreateHook(reinterpret_cast<void*>(base + hook.offset), hook.detour, hook.original) != MH_OK) break;
        ++created;
    }
    bool ok = created == std::size(hooks);
    if (ok) {
        SetPhase(Phase::Ready);
        for (const auto& hook : hooks) if (MH_EnableHook(reinterpret_cast<void*>(base + hook.offset)) != MH_OK) { ok = false; break; }
    }
    if (ok) return;
    // Reverse installation order: stop the game-thread entry first.
    while (created) {
        auto target = reinterpret_cast<void*>(base + hooks[--created].offset);
        const auto result = MH_DisableHook(target);
        if (result == MH_OK || result == MH_ERROR_DISABLED) MH_RemoveHook(target);
    }
    TlsFree(lookupTls); lookupTls = TLS_OUT_OF_INDEXES;
    SetPhase(Phase::HookFailed);
}
Status NativeStatus() { std::lock_guard<std::mutex> lock(statusMutex); return status; }
void TickNativeDiagnostics() {
    static Status previous;
    const auto value = NativeStatus();
    if (value.phase == previous.phase && value.hero == previous.hero && value.revision == previous.revision &&
        value.rebuilds == previous.rebuilds && value.matched == previous.matched && value.wearableLists == previous.wearableLists &&
        value.resyncs == previous.resyncs && !strcmp(value.renderModel, previous.renderModel)) return;
    previous = value;
    CreateDirectoryA("C:\\Temp", nullptr); CreateDirectoryA("C:\\Temp\\opencode", nullptr);
    RotateLogIfLarge("C:\\Temp\\opencode\\wardrobe_appearance.log");
    FILE* file = nullptr;
    fopen_s(&file, "C:\\Temp\\opencode\\wardrobe_appearance.log", "a");
    if (!file) return;
    fprintf(file, "%llu pid=%lu phase=%u hero=%u revision=%llu matched=%u/%u attempts=%u rebuilds=%llu gathers=%llu views=%u lists=%llu prepareMs=%u commitMs=%u registered=%u known=%u unavailable=%u resyncs=%u base=%s model=%s render=%s %s\n",
        GetTickCount64(), GetCurrentProcessId(), unsigned(value.phase), value.hero, value.revision,
        value.matched, value.expected, value.attempts, value.rebuilds, value.gathers, value.views, value.wearableLists,
        value.prepareMs, value.commitMs, value.registered, value.known, value.unavailable, value.resyncs,
        value.baseModel, value.selectedModel, value.renderModel, PhaseText(value.phase));
    fclose(file);
}
const char* PhaseText(Phase phase) {
    switch (phase) {
    case Phase::Starting: return "Vérification de Dota...";
    case Phase::Unsupported: return "client.dll non reconnu : apparence en partie désactivée (voir le profil ci-dessous)";
    case Phase::Ready: return "Prêt - équipe une tenue dans Dota, puis choisis ton héros";
    case Phase::Pending: return "Chargement de la tenue du héros...";
    case Phase::Prepared: return "Tenue transmise au moteur de Dota";
    case Phase::MissingItems: return "Tenue indisponible - récupération automatique avec délai entre les tentatives";
    default: return "Connexion au moteur indisponible";
    }
}
}
