#include "../src/native_appearance.cpp"
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID*) { return MH_OK; }
MH_STATUS WINAPI MH_EnableHook(LPVOID) { return MH_OK; }
MH_STATUS WINAPI MH_DisableHook(LPVOID) { return MH_OK; }
MH_STATUS WINAPI MH_RemoveHook(LPVOID) { return MH_OK; }

static unsigned checks = 0, thinkCalls = 0;
static bool missing = false;
static appearance::RenderSlot wanted{12345, 42, 2}, rendered;
static uint64_t lookupId = 0;
static appearance::RenderSlot* createdSelection = nullptr;
static unsigned createdCalls = 0, destroyedCalls = 0, freedLists = 0;
static std::string modelName;
static unsigned spawnCalls = 0;
static int32_t ownerId = 0;
static int32_t* __fastcall WearablePlayer(void*, int32_t* result) { *result = ownerId; return result; }
static void __fastcall SpawnList(void*, void*, void*, appearance::NativeVector* output) { ++spawnCalls; output->count = 1; }
static void __fastcall BuildList(void* hero, void* kv, void* resources, appearance::NativeVector* output) {
    appearance::OnSpawnList(hero, kv, resources, output);
    if (!output->count) {
        auto inventory = appearance::OnPlayerInventory(nullptr,0,false);
        createdSelection = static_cast<appearance::RenderSlot*>(appearance::OnEquipped(inventory,49,0,false));
        output->count = createdSelection ? 2 : 0;
    }
}
static void* __fastcall PlayerInventory(void*, int32_t, bool) { return nullptr; }
static void* __fastcall EquippedView(void*, uint32_t, uint32_t, bool) { return nullptr; }
static void* __fastcall FindItem(void* inventory, uint64_t id, int32_t*) {
    lookupId = id;
    if (missing || uintptr_t(inventory) != appearance::base + appearance::profile::LocalInventory || id != wanted.item) return nullptr;
    return &wanted;
}
static void* __fastcall DefaultView(void*, uint32_t, uint32_t) { static appearance::RenderSlot result{0,10,0}; return &result; }
static void Check(bool ok, const char* text) { ++checks; if (!ok) throw std::runtime_error(text); }
// A rebuild spreads model requests over frames (two per frame): pump a few thinks.
static void Settle(void* hero) { Sleep(85); for (int i = 0; i < 3; ++i) appearance::OnThink(hero); }
static void __fastcall Think(void* hero) {
    ++thinkCalls;
    auto& flags = *reinterpret_cast<uint8_t*>(uintptr_t(hero) + appearance::profile::DirtyFlags);
    flags = 0;
}
static void* __fastcall Allocate(size_t bytes) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes); }
static void __fastcall Free(void* data) { ++freedLists; HeapFree(GetProcessHeap(), 0, data); }
static void* __fastcall KvCtor(void* kv, void*, uint8_t) { return kv; }
static void __fastcall KvDtor(void*) {}
static void __fastcall Prepare(void* hero, void* kv, void* resources) {
    auto list = reinterpret_cast<appearance::NativeVector*>(uintptr_t(hero) + appearance::profile::CreationList);
    appearance::OnBuildList(hero, kv, resources, list);
    if (list->count > 0) {
        list->data = static_cast<void**>(Allocate(size_t(list->count) * sizeof(appearance::CreationEntry)));
        auto entries = reinterpret_cast<appearance::CreationEntry*>(list->data);
        entries[0].view = createdSelection;
        *reinterpret_cast<uint8_t*>(uintptr_t(hero) + appearance::profile::CreationInitialized) = 1;
    }
}
static void __fastcall Create(void* hero, void*) {
    ++createdCalls;
    auto list = reinterpret_cast<appearance::NativeVector*>(uintptr_t(hero) + appearance::profile::CreationList);
    *reinterpret_cast<int32_t*>(uintptr_t(hero) + 0xad8) = list->count;
    rendered = *static_cast<appearance::RenderSlot*>(reinterpret_cast<appearance::CreationEntry*>(list->data)[0].view);
}
static void __fastcall Destroy(void* hero) { ++destroyedCalls; *reinterpret_cast<int32_t*>(uintptr_t(hero) + 0xad8) = 0; }
static void __fastcall ModifierDtor(void* data, uint32_t) { HeapFree(GetProcessHeap(), 0, data); }
static void* __fastcall ModifierCtor(void* data) {
    static void* table[] = {reinterpret_cast<void*>(ModifierDtor)};
    *static_cast<void***>(data) = table;
    *reinterpret_cast<LONG*>(uintptr_t(data) + 8) = 1; return data;
}
static void* __fastcall ModifierManager() { return nullptr; }
static void __fastcall Populate(void*, void*, int32_t count, void** views) { Check(count > 0 && views[0], "Native modifiers receive the filtered creation views"); }
static void* __fastcall ModelOverride(void*, void*) {
    struct Override { void* unused[2]; const char* path; }; static Override model{{}, "persona.vmdl"};
    return createdSelection && createdSelection->item ? &model : nullptr;
}
static std::array<uint8_t, 0x28> personaResource{}, defaultResource{}, wearableResource{}, errorResource{}, dragonResource{};
static void* ResourceFor(const char* path);
static void* currentModel = nullptr;
static void __fastcall SetModel(void*, const char* model) { modelName = model; currentModel = ResourceFor(model); }
static void* __fastcall ModelHandle(void*, void** handle) { *handle = currentModel ? currentModel : reinterpret_cast<void*>(1); return handle; }
static void __fastcall ModelName(void*, void* handle, char* output, uint32_t size) {
    strcpy_s(output, size, handle == personaResource.data() ? "persona.vmdl" : handle == wearableResource.data() ? "wearable.vmdl" :
        handle == errorResource.data() ? "models/dev/error.vmdl" : handle == dragonResource.data() ? "dragon.vmdl" : "default.vmdl");
}
static bool ready = true;
static void* combinedBase = nullptr;
static std::set<std::string> registeredModels;
static unsigned nameCtors = 0, namePurges = 0, manifestAdds = 0, handleFinds = 0;
static void* ResourceFor(const char* path) {
    return !strcmp(path,"default.vmdl") ? defaultResource.data() : !strcmp(path,"persona.vmdl") ? personaResource.data() :
        !strcmp(path,"dragon.vmdl") ? dragonResource.data() : wearableResource.data();
}
// The outfit's entity_model replacements (e.g. a persona's dragon form).
static const char* dragonPath = "dragon.vmdl";
static std::array<uint8_t, 0x40> dragonEntry{};
static void* replacementEntries[1] = {dragonEntry.data()};
static appearance::PointerList replacements{1, 0, replacementEntries};
static void* __fastcall EntityModels(void* modifiers) { Check(modifiers != nullptr, "Replacements are read from the staged modifier list"); return &replacements; }
// Like Dota, an unregistered model resolves to an empty handle bound to models/dev/error.vmdl.
static void* __fastcall LoadModel(void*, void** output, const char* path) {
    *output = registeredModels.count(path) ? ResourceFor(path) : errorResource.data();
    return output;
}
static const char* NamePath(void* name) { return static_cast<appearance::ResourceName*>(name)->storage; }
static void __fastcall NameCtor(void* raw, const char* path) {
    auto name = static_cast<appearance::ResourceName*>(raw); ++nameCtors;
    Check(name->length == 0 && name->allocated == 0xc00000c8 && !name->hash && !name->type, "Resource names are zeroed before the engine fills them");
    strcpy_s(name->storage, path); name->length = int32_t(strlen(path)); name->hash = 0x1234;
}
static bool __fastcall NameIsType(void* name, uint32_t type) {
    const auto path = NamePath(name); const auto length = strlen(path);
    return type == appearance::ModelResourceType && length > 5 && !strcmp(path + length - 5, ".vmdl");
}
static void __fastcall NamePurge(void*, int32_t) { ++namePurges; }
static int32_t __fastcall ResourceState(void*, appearance::ResourceName* name) { return registeredModels.count(NamePath(name)) ? 3 : 0; }
static void* __fastcall ResourceRegister(void*, appearance::ResourceName* name, const char* manifest) {
    Check(!strcmp(manifest, "JustInTimeManifest"), "Unknown models join Dota's just-in-time manifest");
    ++manifestAdds; registeredModels.insert(NamePath(name)); return ResourceFor(NamePath(name));
}
static void* __fastcall ResourceFind(void*, appearance::ResourceName* name, bool create) {
    Check(!create, "Known models are looked up without creating empty handles");
    ++handleFinds; return registeredModels.count(NamePath(name)) ? ResourceFor(NamePath(name)) : nullptr;
}
static void __fastcall ReleaseModel(void*, void*) {}
static const char* __fastcall KvModel(void*, const char*) { return "default.vmdl"; }
static void __fastcall SetBaseModel(void*, void* model) { combinedBase = model; }
static bool __fastcall ModelReady(void*) { return ready; }
static const char* __fastcall ViewModel(void*, int32_t) { return "wearable.vmdl"; }

int main() {
    using namespace appearance;
    try {
        Scheduler burst;
        Check(burst.Observe(1, 11, 1000) && !burst.Begin(1074), "Leave time for the delivered GC reply to be processed");
        Check(burst.Observe(1, 12, 1070) && !burst.Begin(1144) && burst.Begin(1145), "Rapid equipment changes coalesce to the latest outfit");
        burst.Complete();
        Check(!burst.Observe(1, 12, 2000) && !burst.Begin(2000), "Repeated clicks with identical appearance do not rebuild the model");
        Check(burst.Observe(2, 12, 2000) && burst.Begin(2075), "A new hero entity with the same outfit is updated again");
        Check(!burst.Begin(2100) && burst.Begin(2325) && burst.Begin(2575) && burst.Begin(2825), "Retries are spaced across frames");
        Check(burst.Exhausted() && !burst.Begin(9999999), "Missing inventory cannot cause an infinite rebuild loop");
        auto snapshot = std::make_shared<Snapshot>();
        snapshot->steamId = 123; snapshot->revision = 1; snapshot->selections.push_back({49, 0, 12345, 42, 2});
        const auto fingerprint = HeroFingerprint(*snapshot, 49);
        snapshot->selections.push_back({50, 1, 999, 100, 0}); snapshot->revision++;
        Check(HeroFingerprint(*snapshot, 49) == fingerprint, "Another hero's equipment does not invalidate the current model");
        RenderSlot slots[32]{}; slots[0] = wanted;
        Check(MatchingSelections(*snapshot, 49, slots) == 1, "Definition and style must match the selected appearance");
        slots[0].style = 0;
        Check(MatchingSelections(*snapshot, 49, slots) == 0, "A wrong style must not be reported as prepared");
        auto defaults = *snapshot; defaults.selections[0] = {49, 0, 0, 0, 0};
        Check(!MatchingSelections(defaults, 49, slots), "A default selection must not accept the previous non-default item");
        slots[0] = {0, 10, 0};
        Check(MatchingSelections(defaults, 49, slots) == 1, "Dota resolves the actual definition of a default slot");

        InitializeNative();
        Check(NativeStatus().phase == Phase::Unsupported, "No client module must leave native writes disabled");
        Check(!DiskMatches(GetModuleHandleW(nullptr)), "An unrelated executable cannot match the Dota profile");
        base = uintptr_t(VirtualAlloc(nullptr, profile::ImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Check(base && !MemoryMatches(), "Malformed image headers are rejected before any hooks");
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base); dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 128;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + 128); nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64; nt->FileHeader.TimeDateStamp = profile::Timestamp;
        nt->OptionalHeader.SizeOfImage = profile::ImageSize;
        memcpy(reinterpret_cast<void*>(base + profile::Think), profile::ThinkBytes, sizeof(profile::ThinkBytes));
        memcpy(reinterpret_cast<void*>(base + profile::GatherNetwork), profile::NetworkBytes, sizeof(profile::NetworkBytes));
        memcpy(reinterpret_cast<void*>(base + profile::GatherInventory), profile::InventoryBytes, sizeof(profile::InventoryBytes));
        memcpy(reinterpret_cast<void*>(base + profile::InventoryForPlayer), profile::PlayerInventoryBytes, sizeof(profile::PlayerInventoryBytes));
        memcpy(reinterpret_cast<void*>(base + profile::EquippedView), profile::EquippedBytes, sizeof(profile::EquippedBytes));
        memcpy(reinterpret_cast<void*>(base + profile::BuildWearableList), profile::BuildListBytes, sizeof(profile::BuildListBytes));
        memcpy(reinterpret_cast<void*>(base + profile::BuildSpawnWearableList), profile::SpawnListBytes, sizeof(profile::SpawnListBytes));
        memcpy(reinterpret_cast<void*>(base + profile::PrepareWearables), profile::PrepareBytes, sizeof(profile::PrepareBytes));
        memcpy(reinterpret_cast<void*>(base + profile::CreateWearables), profile::CreateBytes, sizeof(profile::CreateBytes));
        Check(MemoryMatches(), "The verified profile accepts the corresponding runtime entry bytes");
        *reinterpret_cast<uint8_t*>(base + profile::GatherInventory) ^= 1;
        Check(!MemoryMatches(), "A changed function entry disables the profile even when the PE version matches");

        std::array<uint8_t, 0x2000> hero{}, otherHero{};
        std::array<uint8_t, 0x1000> controller{};
        std::array<uintptr_t, 64> chunks{};
        std::array<uint8_t, 0x70 * 512> identities{};
        *reinterpret_cast<void**>(base + profile::LocalController) = controller.data();
        *reinterpret_cast<void**>(base + profile::EntityChunks) = chunks.data();
        *reinterpret_cast<uint64_t*>(controller.data() + profile::SteamId) = 123;
        *reinterpret_cast<uint32_t*>(controller.data() + profile::AssignedHero) = 0x8005;
        *reinterpret_cast<uint32_t*>(hero.data() + profile::HeroId) = 49;
        chunks[0] = uintptr_t(identities.data());
        *reinterpret_cast<void**>(identities.data() + 5 * 0x70) = hero.data();
        *reinterpret_cast<uint32_t*>(identities.data() + 5 * 0x70 + 0x10) = 0x8005;
        *reinterpret_cast<void**>(hero.data() + 0x10) = identities.data() + 5 * 0x70;
        uint32_t id = 0, handle = 0;
        Check(LocalHero(hero.data(), 123, id, handle) && id == 49 && handle == 0x8005, "Resolve the exact local hero and full handle serial");
        Check(!LocalHero(otherHero.data(), 123, id, handle) && !LocalHero(hero.data(), 124, id, handle), "Remote heroes and other accounts cannot be overridden");
        *reinterpret_cast<uint32_t*>(identities.data() + 5 * 0x70 + 0x10) = 5;
        Check(!LocalHero(hero.data(), 123, id, handle), "A recycled entity index with a different serial must be rejected");
        *reinterpret_cast<uint32_t*>(identities.data() + 5 * 0x70 + 0x10) = 0x8005;
        originalThink = Think;
        originalPlayerInventory = PlayerInventory; originalEquipped = EquippedView; findItem = FindItem; defaultView = DefaultView;
        Check(OnPlayerInventory(nullptr,0,false) == nullptr && OnEquipped(nullptr,49,0,false) == nullptr,
            "Outside the local appearance gather, inventory and loadout lookups remain original");
        Publish(snapshot);
        lookupTls = TlsAlloc(); wearablePlayer = WearablePlayer; originalBuildList = BuildList; originalSpawnList = SpawnList;
        NativeVector createdViews{};
        OnBuildList(hero.data(), nullptr, nullptr, &createdViews);
        Check(createdViews.count == 2 && !spawnCalls && !ActiveList(), "Native wearable creation takes the acknowledged inventory and restores the scoped context");
        ownerId = 1; createdViews.count = 0;
        OnBuildList(hero.data(), nullptr, nullptr, &createdViews);
        Check(createdViews.count == 1 && spawnCalls == 1, "Another player's spawn-KV wearables are preserved");
        ownerId = 0; missing = true; createdViews.count = 0;
        OnBuildList(hero.data(), nullptr, nullptr, &createdViews);
        Check(createdViews.count == 1 && spawnCalls == 2, "Missing views preserve native spawn equipment");
        missing = false;
        engine = {Allocate, Free, KvCtor, KvDtor, Prepare, Create, Destroy, ModifierCtor, ModifierManager, Populate, ModelOverride, SetModel, ModelHandle,
            KvModel, SetBaseModel, ModelReady, ViewModel};
        void* modelVtable[14]{}; modelVtable[13] = reinterpret_cast<void*>(ModelName); modelVtable[12] = reinterpret_cast<void*>(LoadModel);
        void** modelObject = modelVtable;
        *reinterpret_cast<void**>(base + profile::ModelInfo) = &modelObject;
        void* resourceVtable[3]{}; resourceVtable[2] = reinterpret_cast<void*>(ReleaseModel); void** resourceObject = resourceVtable;
        *reinterpret_cast<void**>(base + profile::ResourceSystem) = &resourceObject;
        void* systemVtable[80]{}; systemVtable[profile::ResourceStateSlot] = reinterpret_cast<void*>(ResourceState);
        systemVtable[profile::ResourceRegisterSlot] = reinterpret_cast<void*>(ResourceRegister); systemVtable[profile::ResourceFindSlot] = reinterpret_cast<void*>(ResourceFind);
        void** systemObject = systemVtable;
        *reinterpret_cast<void**>(base + profile::ResourceSystemInterface) = &systemObject;
        strcpy_s(reinterpret_cast<char*>(base + profile::JustInTimeManifest), 32, "JustInTimeManifest");
        engine.nameCtor = NameCtor; engine.nameIsType = NameIsType; engine.namePurge = NamePurge; engine.entityModels = EntityModels;
        *reinterpret_cast<const char**>(dragonEntry.data() + 0x10) = dragonPath; currentModel = defaultResource.data();
        CaptureSpawnModel(hero.data(), reinterpret_cast<void*>(1));
        Check(baselineEntity == EntityKey(hero.data(), 0x8005) && !strcmp(baselineModel.data(), "default.vmdl"),
            "Capture the original spawn model using the full entity identity before applying a persona");
        OnThink(hero.data());
        Check(thinkCalls == 1 && !createdCalls, "The first frame preserves the engine's normal think and defers appearance work");
        Settle(hero.data());
        Check(createdCalls == 1 && rendered.definition == 42 && rendered.style == 2 && NativeStatus().phase == Phase::Prepared && modelName == "persona.vmdl",
            "The native think path rebuilds the wearable entities and the persona skeleton together");
        Check(combinedBase == personaResource.data() && combinedBase != errorResource.data(),
            "A persona base model no server precached is registered just in time instead of resolving to the error model");
        Check(registeredModels.count("persona.vmdl") && registeredModels.count("wearable.vmdl") && registeredModels.count("dragon.vmdl") &&
            manifestAdds == 3 && NativeStatus().registered == 3 && !NativeStatus().unavailable,
            "The persona skeleton, its wearables and its transformation models are registered before loading");
        Check(*reinterpret_cast<LONG*>(dragonResource.data() + 0x20) == 0 && staged.paths.empty(),
            "Preloaded transformation models are released with the rest of the stage");
        Check(nameCtors == 3 && namePurges == 3, "Every resource name built for registration is purged");
        verifyAt = 1; OnThink(hero.data());
        Check(!verifyAt && checkAt && !strcmp(NativeStatus().renderModel, "persona.vmdl"), "After a commit the entity's rendered model is read back for diagnostics");
        checkAt = 1; OnThink(hero.data());
        Check(NativeStatus().phase == Phase::Prepared && !NativeStatus().resyncs, "A rendered model derived from the committed outfit needs no resync");
        Check(lookupId == 12345, "Resolve the exact acknowledged item ID from the local inventory, independent of the match inventory index");
        for (unsigned i = 0; i < 1000; ++i) OnThink(hero.data());
        Check(createdCalls == 1, "A stable outfit does no model reconstruction across 1000 subsequent frames");
        auto next = std::make_shared<Snapshot>(*snapshot); ++next->revision;
        next->selections[0].definition = 43; next->selections[0].item = 12346; Publish(next);
        OnThink(hero.data());
        *reinterpret_cast<uint8_t*>(hero.data() + profile::DirtyFlags) |= 4;
        OnThink(hero.data());
        Check(!scheduler.CompleteState() && NativeStatus().phase == Phase::Pending,
            "A natural refresh of the previous outfit cannot complete the pending new selection");
        wanted = {12346, 43, 2}; Settle(hero.data());
        Check(rendered.definition == 43 && NativeStatus().phase == Phase::Prepared, "A new equip replaces the current outfit rather than applying one click late");
        missing = true;
        next = std::make_shared<Snapshot>(*next); ++next->revision; next->selections[0].definition = 44; Publish(next);
        OnThink(hero.data()); Settle(hero.data());
        Check(rendered.definition == 43 && createdCalls == 2 && destroyedCalls == 2 && NativeStatus().phase == Phase::Pending,
            "Unavailable item views preserve the visible outfit without destroying or rebuilding it");
        const auto callsBefore = createdCalls;
        OnThink(otherHero.data());
        Check(createdCalls == callsBefore, "Other heroes always retain their original appearance");
        missing = false; next = std::make_shared<Snapshot>(*next); ++next->revision; next->selections[0] = {49,0,0,0,0}; Publish(next);
        OnThink(hero.data()); Settle(hero.data());
        Check(modelName == "default.vmdl" && rendered.item == 0 && createdCalls == 3 && freedLists == 2,
            "Unequipping restores the captured base model and releases replaced engine-owned vectors");
        Check(combinedBase == defaultResource.data(), "Leaving a persona also restores the native model combiner's base resource");
        Check(manifestAdds == 4 && NativeStatus().known >= 2 && nameCtors == namePurges, "Models already known to the resource system are found rather than registered again");
        ready = false;
        next = std::make_shared<Snapshot>(*next); ++next->revision; next->selections[0] = {49,0,wanted.item,wanted.definition,wanted.style}; Publish(next);
        OnThink(hero.data()); Settle(hero.data());
        Check(staged.list.data && createdCalls == 3 && destroyedCalls == 3 && modelName == "default.vmdl",
            "Loading the next outfit preserves the visible equipment until all models are ready");
        for (unsigned i = 0; i < 100; ++i) OnThink(hero.data());
        Check(scheduler.Attempts() == 1 && createdCalls == 3, "Resource polling consumes no rebuild attempts and performs no visible work");
        next = std::make_shared<Snapshot>(*next); ++next->revision; next->selections[0] = {49,0,0,0,0}; Publish(next);
        OnThink(hero.data());
        Check(!staged.list.data && createdCalls == 3, "A newer equip discards an unfinished outfit before it can appear one click late");
        ready = true; Settle(hero.data());
        Check(modelName == "default.vmdl" && createdCalls == 4, "The latest selection wins when resource loading finishes");
        verifyAt = 1; OnThink(hero.data());
        currentModel = personaResource.data(); checkAt = 1; OnThink(hero.data());
        Check(NativeStatus().resyncs == 1 && NativeStatus().phase == Phase::Pending && createdCalls == 4,
            "A server-driven model change away from the committed outfit schedules one re-application");
        Settle(hero.data());
        Check(createdCalls == 5 && currentModel == defaultResource.data() && NativeStatus().phase == Phase::Prepared,
            "The resync re-applies the same outfit through the normal staged path");
        for (unsigned i = 0; i < 5; ++i) {
            verifyAt = 1; OnThink(hero.data()); currentModel = errorResource.data(); checkAt = 1; OnThink(hero.data());
            Settle(hero.data());
        }
        Check(NativeStatus().resyncs == Scheduler::MaxResyncs && createdCalls == 7, "A persistent mismatch is bounded per outfit and cannot loop");
        Scheduler resync; resync.Observe(1, 1, 0);
        Check(!resync.Retry(0), "No resync before the outfit was committed");
        resync.Complete();
        Check(resync.Retry(10) && !resync.Begin(50) && resync.Begin(85) && resync.Resyncs() == 1, "A resync waits like a fresh equip before rebuilding");
        resync.Complete(); resync.Retry(200); resync.Complete(); resync.Retry(300); resync.Complete();
        Check(!resync.Retry(400) && resync.Observe(1, 2, 500) && !resync.Resyncs(), "A new outfit restores the resync budget");
        Check(*reinterpret_cast<LONG*>(wearableResource.data()+0x20) == 0 && *reinterpret_cast<LONG*>(personaResource.data()+0x20) == 0,
            "Completed and cancelled stages release their model resource references");
        ReleaseCreationList(*reinterpret_cast<NativeVector*>(hero.data() + profile::CreationList));
        ReleaseModifiers(*reinterpret_cast<void**>(hero.data() + profile::Modifiers));
        VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE); base = 0;
        TlsFree(lookupTls); lookupTls = TLS_OUT_OF_INDEXES;
        std::cout << "PASS: " << checks << " native appearance, identity, profile and scheduling checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
