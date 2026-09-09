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
constexpr uint32_t ModelResourceType = 0x6c646d76; // 'vmdl'
std::array<char, 264> baselineModel{};
uint64_t baselineEntity = 0;
struct StagedOutfit {
    NativeVector list{};
    void* modifiers = nullptr;
    std::vector<std::string> paths;
    std::vector<void*> models;
    uint64_t started = 0;
} staged;
// After a commit, read back which model the entity actually renders, then
// keep checking it: the server owns the networked model, so the end of a
// transformation or a full entity update can put the classic model back.
uint64_t verifyAt = 0, checkAt = 0;
constexpr uint64_t VerifyDelayMs = 2000, CheckIntervalMs = 500;
std::vector<std::string> committedStems;
void RememberCommitted(const std::vector<std::string>& paths) {
    committedStems.clear();
    for (auto stem : paths) {
        const auto extension = stem.rfind(".vmdl");
        if (extension != std::string::npos && extension + 5 == stem.size()) stem.erase(extension);
        if (!stem.empty()) committedStems.push_back(std::move(stem));
    }
}
// Combined meshes are named after their base ("<base>_c_<n>.vmdl") and a
// transformation switches to another replacement of the same outfit; both
// count as ours. Anything else, including models/dev/error.vmdl, does not.
bool RenderMatchesCommitted(const char* name) {
    if (!*name || committedStems.empty()) return true;
    for (const auto& stem : committedStems) if (!strncmp(name, stem.c_str(), stem.size())) return true;
    return false;
}
using ThinkFn = void(__fastcall*)(void*);
using PlayerInventoryFn = void*(__fastcall*)(void*, int32_t, bool);
using EquippedFn = void*(__fastcall*)(void*, uint32_t, uint32_t, bool);
using FindItemFn = void*(__fastcall*)(void*, uint64_t, int32_t*);
using DefaultFn = void*(__fastcall*)(void*, uint32_t, uint32_t);
using BuildListFn = void(__fastcall*)(void*, void*, void*, NativeVector*);
using WearablePlayerFn = int32_t*(__fastcall*)(void*, int32_t*);
uintptr_t base = 0;
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

bool MemoryMatches() {
    __try {
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return false;
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
            nt->FileHeader.TimeDateStamp == profile::Timestamp && nt->OptionalHeader.SizeOfImage == profile::ImageSize &&
            !memcmp(reinterpret_cast<void*>(base + profile::Think), profile::ThinkBytes, sizeof(profile::ThinkBytes)) &&
            !memcmp(reinterpret_cast<void*>(base + profile::GatherNetwork), profile::NetworkBytes, sizeof(profile::NetworkBytes)) &&
            !memcmp(reinterpret_cast<void*>(base + profile::GatherInventory), profile::InventoryBytes, sizeof(profile::InventoryBytes)) &&
            !memcmp(reinterpret_cast<void*>(base + profile::InventoryForPlayer), profile::PlayerInventoryBytes, sizeof(profile::PlayerInventoryBytes)) &&
            !memcmp(reinterpret_cast<void*>(base + profile::BuildWearableList), profile::BuildListBytes, sizeof(profile::BuildListBytes)) &&
            !memcmp(reinterpret_cast<void*>(base + profile::BuildSpawnWearableList), profile::SpawnListBytes, sizeof(profile::SpawnListBytes)) &&
            !memcmp(reinterpret_cast<void*>(base + profile::PrepareWearables), profile::PrepareBytes, sizeof(profile::PrepareBytes)) &&
            !memcmp(reinterpret_cast<void*>(base + profile::CreateWearables), profile::CreateBytes, sizeof(profile::CreateBytes)) &&
            !memcmp(reinterpret_cast<void*>(base + profile::EquippedView), profile::EquippedBytes, sizeof(profile::EquippedBytes));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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
        const auto identity = chunk + (index & 0x1ff) * 0x70;
        if (*reinterpret_cast<uint32_t*>(identity + 0x10) != handle || *reinterpret_cast<void**>(identity) != hero) return false;
        heroId = *reinterpret_cast<uint32_t*>(uintptr_t(hero) + profile::HeroId);
        return heroId > 0 && heroId < 1000;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* __fastcall OnPlayerInventory(void* manager, int32_t player, bool local) {
    // In a match, the player-resource inventory can differ from the local
    // menu inventory. Redirect only inside our local hero appearance gather.
    if (ActiveList()) return reinterpret_cast<void*>(base + profile::LocalInventory);
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
            return findItem(reinterpret_cast<void*>(base + profile::LocalInventory), selection.item, nullptr);
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
        if (owner < 0 || owner >= 64 || owner != *reinterpret_cast<int32_t*>(controller + 0x908)) return false;
        heroId = *reinterpret_cast<uint32_t*>(uintptr_t(hero) + profile::HeroId);
        return heroId > 0 && heroId < 1000;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CaptureBaseline(void* hero, uint64_t entity);
uint64_t EntityKey(void* hero, uint32_t handle) {
    return uint64_t(uintptr_t(hero)) ^ (uint64_t(handle) << 32);
}
void CaptureSpawnModel(void* hero, void* kv) {
    const auto identity = *reinterpret_cast<uintptr_t*>(uintptr_t(hero) + 0x10);
    if (!identity || *reinterpret_cast<void**>(identity) != hero) return;
    const auto entity = EntityKey(hero, *reinterpret_cast<uint32_t*>(identity + 0x10));
    // Capture before inventory modifiers can replace the render/base model.
    // Empty KVs from a live update must never overwrite this spawn baseline.
    const char* model = kv && engine.kvModel ? engine.kvModel(kv, nullptr) : nullptr;
    if (model && *model) {
        strncpy_s(baselineModel.data(), baselineModel.size(), model, _TRUNCATE);
        baselineEntity = entity;
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
        if (!findItem(reinterpret_cast<void*>(base + profile::LocalInventory), selection.item, nullptr)) {
            originalBuildList(hero, kv, resources, output); return;
        }
    }
    CaptureSpawnModel(hero, kv);
    ListContext context{snapshot, hero, heroId, output};
    ScopedList scope(context);
    originalBuildList(hero, kv, resources, output);
    std::lock_guard<std::mutex> lock(statusMutex);
    ++status.wearableLists;
}

void ReleaseCreationList(NativeVector& list) {
    auto entries = reinterpret_cast<CreationEntry*>(list.data);
    for (int32_t i = 0; i < list.count; ++i) if (entries[i].owned && entries[i].view) {
        auto destructor = reinterpret_cast<void(__fastcall*)(void*, uint32_t)>((*static_cast<void***>(entries[i].view))[2]);
        destructor(entries[i].view, 1);
    }
    if (list.data && !(uint32_t(list.flags) & 0xc0000000)) engine.free(list.data);
    list = {};
}
void ReleaseModifiers(void* modifiers) {
    if (modifiers && InterlockedDecrement(reinterpret_cast<LONG*>(uintptr_t(modifiers) + 8)) == 0) {
        auto destructor = reinterpret_cast<void(__fastcall*)(void*, uint32_t)>((*static_cast<void***>(modifiers))[0]);
        destructor(modifiers, 1);
    }
}
bool CaptureBaseline(void* hero, uint64_t entity) {
    if (entity == baselineEntity && baselineModel[0]) return true;
    baselineModel.fill(0);
    void* handle = *reinterpret_cast<void**>(uintptr_t(hero) + 0x15e8);
    if (!handle) engine.modelHandle(hero, &handle);
    const auto info = *reinterpret_cast<void**>(base + profile::ModelInfo);
    if (!info || !handle) return false;
    auto name = reinterpret_cast<void(__fastcall*)(void*, void*, char*, uint32_t)>((*static_cast<void***>(info))[13]);
    name(info, handle, baselineModel.data(), uint32_t(baselineModel.size()));
    baselineModel.back() = 0;
    baselineEntity = entity;
    return baselineModel[0] != 0;
}
void ReleaseStaged() {
    ReleaseCreationList(staged.list);
    ReleaseModifiers(staged.modifiers);
    const auto resources = *reinterpret_cast<void**>(base + profile::ResourceSystem);
    for (auto model : staged.models) if (model && resources &&
        InterlockedDecrement(reinterpret_cast<LONG*>(uintptr_t(model) + 0x20)) == 0) {
        auto release = reinterpret_cast<void(__fastcall*)(void*, void*)>((*static_cast<void***>(resources))[2]);
        release(resources, model);
    }
    staged = {};
}
bool StageWearables(void* hero, const Snapshot& snapshot, uint32_t heroId, uint64_t now) {
    for (const auto& s : snapshot.selections) if (s.hero == heroId && s.slot < 32 && s.item &&
        !findItem(reinterpret_cast<void*>(base + profile::LocalInventory), s.item, nullptr)) return false;
    auto& list = *reinterpret_cast<NativeVector*>(uintptr_t(hero) + profile::CreationList);
    auto& initialized = *reinterpret_cast<uint8_t*>(uintptr_t(hero) + profile::CreationInitialized);
    if (list.count < 0 || list.count > 64 || (list.count && !list.data) || !baselineModel[0]) return false;
    ListContext context{&snapshot, hero, heroId, nullptr};
    ScopedList scope(context);

    // Build an engine-owned replacement before releasing the visible outfit.
    // CEntityKeyValues is 0x38 bytes in the verified constructor/destructor.
    alignas(8) std::array<uint8_t, 0x38> kv{}, resources{};
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
    auto modifiers = engine.allocate(0x30);
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
    const char* model = replacement ? *reinterpret_cast<const char**>(uintptr_t(replacement) + 0x10) : nullptr;
    if (!model || !*model) model = baselineModel.data();
    staged.paths.emplace_back(model);
    activeModifiers = previousModifiers;
    staged.modifiers = modifiers;
    const auto modelIndex = *reinterpret_cast<int32_t*>(uintptr_t(hero) + 0x1d0c);
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
                const auto path = entry ? *reinterpret_cast<const char* const*>(entry + 0x10) : nullptr;
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
    if ((uint32_t(name.length) & 0x3fffffff) && engine.nameIsType(&name, ModelResourceType)) {
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
    auto load = reinterpret_cast<void*(__fastcall*)(void*, void**, const char*)>((*static_cast<void***>(info))[12]);
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
        if (model) InterlockedIncrement(reinterpret_cast<LONG*>(uintptr_t(model) + 0x20));
        staged.models.push_back(model);
    }
    if (staged.models.size() != staged.paths.size()) return false;
    for (auto model : staged.models) if (!model || !engine.modelReady(model)) return false;
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
    alignas(8) std::array<uint8_t, 0x38> kv{};
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
    return *reinterpret_cast<int32_t*>(uintptr_t(hero) + 0xad8) > 0;
}

void RecordRenderModel(void* hero, char (&name)[264]) {
    // The HUD reports only native acceptance; this reads what Dota draws.
    const auto info = *reinterpret_cast<void**>(base + profile::ModelInfo);
    void* handle = nullptr;
    if (engine.modelHandle) engine.modelHandle(hero, &handle);
    name[0] = 0;
    if (info && handle) {
        auto text = reinterpret_cast<void(__fastcall*)(void*, void*, char*, uint32_t)>((*static_cast<void***>(info))[13]);
        text(info, handle, name, uint32_t(sizeof(name))); name[sizeof(name) - 1] = 0;
    }
    std::lock_guard<std::mutex> lock(statusMutex);
    strcpy_s(status.renderModel, name);
}
void __fastcall OnThink(void* hero) {
    const auto snapshot = Read();
    uint32_t heroId = 0, handle = 0;
    if (!snapshot || !LocalHero(hero, snapshot->steamId, heroId, handle) || !HeroSelections(*snapshot, heroId)) {
        originalThink(hero); return;
    }
    const auto now = GetTickCount64();
    const uint64_t entity = EntityKey(hero, handle);
    const bool changed = scheduler.Observe(entity, HeroFingerprint(*snapshot, heroId), now);
    if (changed) {
        ReleaseStaged(); verifyAt = checkAt = 0; committedStems.clear();
        std::lock_guard<std::mutex> lock(statusMutex);
        status.phase = Phase::Pending; status.hero = heroId; status.revision = snapshot->revision;
        status.expected = uint32_t(HeroSelections(*snapshot, heroId)); status.matched = 0; status.attempts = 0;
    }
    if (!staged.list.data && scheduler.Begin(now)) {
        if (CaptureBaseline(hero, entity)) StageWearables(hero, *snapshot, heroId, now);
        std::lock_guard<std::mutex> lock(statusMutex); status.attempts = scheduler.Attempts();
    }
    if (staged.list.data && ModelsReady()) {
        const auto begin = GetTickCount64();
        const bool rebuilt = CommitWearables(hero, *snapshot, heroId);
        const auto elapsed = GetTickCount64() - begin;
        RememberCommitted(staged.paths);
        ReleaseStaged(); scheduler.Complete(); verifyAt = now + VerifyDelayMs; checkAt = 0;
        std::lock_guard<std::mutex> lock(statusMutex);
        ++status.rebuilds; status.commitMs = uint32_t(elapsed);
        status.phase = rebuilt ? Phase::Prepared : Phase::MissingItems;
        status.matched = rebuilt ? status.expected : 0;
        status.views = rebuilt ? uint32_t(reinterpret_cast<NativeVector*>(uintptr_t(hero) + profile::CreationList)->count) : 0;
    } else if (staged.list.data && now - staged.started > 10000) {
        ReleaseStaged(); scheduler.Complete(); SetPhase(Phase::MissingItems);
    }
    originalThink(hero);
    if (verifyAt && now >= verifyAt) {
        char name[264]; verifyAt = 0; checkAt = now + CheckIntervalMs; RecordRenderModel(hero, name);
    } else if (checkAt && now >= checkAt && !staged.list.data && scheduler.CompleteState()) {
        char name[264]; checkAt = now + CheckIntervalMs; RecordRenderModel(hero, name);
        if (!RenderMatchesCommitted(name) && scheduler.Retry(now)) {
            std::lock_guard<std::mutex> lock(statusMutex); ++status.resyncs; status.phase = Phase::Pending;
        }
    }
    if (!staged.list.data && scheduler.Exhausted()) SetPhase(Phase::MissingItems);
    // Readable diagnostics stay off the model-loading path; the HUD only
    // reports native input acceptance, not proof of completed visual loading.
    { std::lock_guard<std::mutex> lock(statusMutex); status.lastSeenMs = now; }
}
}

void InitializeNative() {
    const auto module = GetModuleHandleW(L"client.dll"); base = uintptr_t(module);
    if (!base || !DiskMatches(module) || !MemoryMatches()) { SetPhase(Phase::Unsupported); return; }
    findItem = reinterpret_cast<FindItemFn>(base + profile::FindItemView);
    defaultView = reinterpret_cast<DefaultFn>(base + profile::DefaultView);
    wearablePlayer = reinterpret_cast<WearablePlayerFn>(base + profile::WearablePlayer);
#define NATIVE_MEMBER(member, offset) engine.member = reinterpret_cast<decltype(engine.member)>(base + profile::offset)
    NATIVE_MEMBER(allocate, Allocate); NATIVE_MEMBER(free, Free);
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
    case Phase::Unsupported: return "Version de Dota non reconnue : apparence en partie désactivée";
    case Phase::Ready: return "Prêt - équipe une tenue dans Dota, puis choisis ton héros";
    case Phase::Pending: return "Chargement de la tenue du héros...";
    case Phase::Prepared: return "Tenue transmise au moteur de Dota";
    case Phase::MissingItems: return "Tenue indisponible dans le moteur - nouvelle tentative au prochain équipement";
    default: return "Connexion au moteur indisponible";
    }
}
}
