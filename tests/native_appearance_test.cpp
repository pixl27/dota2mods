#include "../src/native_appearance.cpp"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

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
        const auto heroId = *reinterpret_cast<uint32_t*>(uintptr_t(hero) + appearance::profile::HeroId);
        createdSelection = static_cast<appearance::RenderSlot*>(appearance::OnEquipped(inventory,heroId,0,false));
        output->count = createdSelection ? 2 : 0;
    }
}
static void* __fastcall PlayerInventory(void*, int32_t, bool) { return nullptr; }
static void* __fastcall EquippedView(void*, uint32_t, uint32_t, bool) { return nullptr; }
static void* __fastcall FindItem(void* inventory, uint64_t id, int32_t*) {
    lookupId = id;
    if (missing || uintptr_t(inventory) != appearance::LocalInventory() || id != wanted.item) return nullptr;
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
static bool failModelLoad = false;
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
    *output = !failModelLoad && registeredModels.count(path) ? ResourceFor(path) : errorResource.data();
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
    return type == appearance::profile::ModelResourceType && length > 5 && !strcmp(path + length - 5, ".vmdl");
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

        // A synthetic client.dll: every recorded signature and site window placed
        // where the profile expects it, so the resolver must reproduce the profile.
        std::vector<uintptr_t> recordedFunctions, recordedValues;
        for (const auto& signature : profile::Functions) recordedFunctions.push_back(*signature.target);
        for (const auto& site : profile::Sites) recordedValues.push_back(*site.target);
        base = uintptr_t(VirtualAlloc(nullptr, profile::ImageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        Check(base && !profile::Resolver().Run(base).resolved, "Malformed image headers are rejected before any hooks");
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base); dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 128;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + 128); nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64; nt->FileHeader.TimeDateStamp = profile::Timestamp;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->FileHeader.NumberOfSections = 2;
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.SizeOfImage = profile::ImageSize;
        constexpr uint32_t TextRva = 0x1000, DataRva = 0x3b80000;
        auto section = IMAGE_FIRST_SECTION(nt);
        memcpy(section[0].Name, ".text", 6);
        section[0].VirtualAddress = TextRva; section[0].Misc.VirtualSize = DataRva - TextRva;
        section[0].Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
        memcpy(section[1].Name, ".rdata", 7);
        section[1].VirtualAddress = DataRva; section[1].Misc.VirtualSize = profile::ImageSize - DataRva;
        section[1].Characteristics = IMAGE_SCN_MEM_READ;
        auto place = [&](uintptr_t rva, const uint8_t* bytes, size_t size) { memcpy(reinterpret_cast<void*>(base + rva), bytes, size); };
        for (const auto& signature : profile::Functions) place(*signature.target, signature.bytes, signature.size);
        for (const auto& site : profile::Sites)
            place(site.kind == profile::SiteKind::Schema ? site.offset : *site.function + site.offset, site.bytes, site.size);
        for (const auto& site : profile::Sites)
            if (site.text) strcpy_s(reinterpret_cast<char*>(base + *site.target), 64, site.text);

        auto resolution = profile::Resolver().Run(base);
        Check(resolution.resolved && !resolution.searched && !resolution.moved,
            "A client.dll laid out as recorded resolves without searching for anything");
        for (size_t i = 0; i < std::size(profile::Functions); ++i)
            Check(*profile::Functions[i].target == recordedFunctions[i], "Every function resolves to its recorded address");
        for (size_t i = 0; i < std::size(profile::Sites); ++i)
            Check(*profile::Sites[i].target == recordedValues[i], "Every global, struct offset and vtable slot re-reads to its recorded value");

        // An update that only moves code: the signature is found at its new address.
        const uintptr_t recordedThink = profile::Think, movedThink = 0x2000000;
        auto moveThink = [&](uintptr_t to) {
            memset(reinterpret_cast<void*>(base + profile::Think), 0, sizeof(profile::ThinkBytes));
            if (to) place(to, profile::ThinkBytes, sizeof(profile::ThinkBytes));
        };
        moveThink(movedThink);
        resolution = profile::Resolver().Run(base);
        Check(resolution.resolved && profile::Think == movedThink && resolution.moved == 1 && resolution.searched == 1,
            "A function that moved is found again by its signature, and only it is reported as moved");
        moveThink(0);
        profile::Think = recordedThink;
        place(profile::Think, profile::ThinkBytes, sizeof(profile::ThinkBytes));

        // A signature that is present twice cannot be trusted to be the right one.
        moveThink(movedThink);
        place(movedThink + 0x1000, profile::ThinkBytes, sizeof(profile::ThinkBytes));
        profile::Think = recordedThink;
        resolution = profile::Resolver().Run(base);
        Check(!resolution.resolved && strstr(resolution.failure, "Think") && strstr(resolution.failure, "ambiguous"),
            "An ambiguous signature names itself and disables the integration instead of guessing");
        memset(reinterpret_cast<void*>(base + movedThink), 0, sizeof(profile::ThinkBytes));
        memset(reinterpret_cast<void*>(base + movedThink + 0x1000), 0, sizeof(profile::ThinkBytes));
        place(profile::Think, profile::ThinkBytes, sizeof(profile::ThinkBytes));

        // A schema field is re-read from the declaration next to its name.
        const auto& steamId = *std::find_if(std::begin(profile::Sites), std::end(profile::Sites),
            [](const profile::Site& s) { return s.kind == profile::SiteKind::Schema && !strcmp(s.name, "SteamId"); });
        constexpr uintptr_t NameRva = 0x3c00000, DeclarationRva = 0x2100000;
        memset(reinterpret_cast<void*>(base + steamId.offset), 0, steamId.size);
        strcpy_s(reinterpret_cast<char*>(base + NameRva), 32, steamId.text);
        place(DeclarationRva, steamId.bytes, steamId.size);
        const uintptr_t reference = DeclarationRva + steamId.size + 1;   // 48 8d 15 <rel32>
        auto declaration = reinterpret_cast<uint8_t*>(base + DeclarationRva + steamId.size);
        declaration[0] = 0x48; declaration[1] = 0x8d; declaration[2] = 0x15;
        const int32_t relative = int32_t(NameRva - (reference + 6));
        memcpy(declaration + 3, &relative, sizeof(relative));
        profile::SteamId = 0;
        resolution = profile::Resolver().Run(base);
        Check(resolution.resolved && profile::SteamId == recordedValues[std::distance(std::begin(profile::Sites), &steamId)],
            "A schema field offset is recovered from the declaration beside its name when the code moved");
        memset(reinterpret_cast<void*>(base + DeclarationRva), 0, steamId.size + 7);
        place(steamId.offset, steamId.bytes, steamId.size);

        // A site whose window is gone fails by name rather than resolving to rubbish.
        const auto& creation = *std::find_if(std::begin(profile::Sites), std::end(profile::Sites),
            [](const profile::Site& s) { return !strcmp(s.name, "CreationList"); });
        memset(reinterpret_cast<void*>(base + *creation.function + creation.offset), 0, creation.size);
        resolution = profile::Resolver().Run(base);
        Check(!resolution.resolved && strstr(resolution.failure, "CreationList"),
            "A site the update removed names itself instead of yielding a wrong offset");
        place(*creation.function + creation.offset, creation.bytes, creation.size);
        Check(profile::Resolver().Run(base).resolved, "Restoring the window resolves the profile again");

        // A call that merely repeats a unique signature may confirm it, never replace it.
        const auto& call = *std::find_if(std::begin(profile::Sites), std::end(profile::Sites),
            [](const profile::Site& s) { return s.kind == profile::SiteKind::Call && s.verify; });
        const uintptr_t callWindow = *call.function + call.offset;
        const uintptr_t honest = *call.target;
        int32_t original = 0;
        memcpy(&original, reinterpret_cast<void*>(base + callWindow + call.operand), sizeof(original));
        const int32_t elsewhere = int32_t(0x2500000 - (callWindow + call.end));
        memcpy(reinterpret_cast<void*>(base + callWindow + call.operand), &elsewhere, sizeof(elsewhere));
        resolution = profile::Resolver().Run(base);
        Check(!resolution.resolved && strstr(resolution.failure, "disagrees") && *call.target == honest,
            "A call site that points somewhere else than the signature fails instead of overwriting it");
        memcpy(reinterpret_cast<void*>(base + callWindow + call.operand), &original, sizeof(original));
        Check(profile::Resolver().Run(base).resolved && *call.target == honest,
            "The signature keeps the address once the call agrees with it again");

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
        Check(*reinterpret_cast<LONG*>(dragonResource.data() + 0x20) == 1 && staged.paths.empty(),
            "Transformation resources stay referenced while the outfit is equipped");
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
        currentModel = errorResource.data(); checkAt = 1; OnThink(hero.data());
        Check(NativeStatus().resyncs == 1 && NativeStatus().phase == Phase::Pending && createdCalls == 4,
            "A server-driven model change away from the committed outfit schedules one re-application");
        Settle(hero.data());
        Check(createdCalls == 5 && currentModel == defaultResource.data() && NativeStatus().phase == Phase::Prepared,
            "The resync re-applies the same outfit through the normal staged path");
        for (unsigned i = 0; i < 5; ++i) {
            verifyAt = 1; OnThink(hero.data()); currentModel = errorResource.data(); checkAt = 1; OnThink(hero.data());
            Settle(hero.data());
        }
        Check(NativeStatus().resyncs == Scheduler::MaxResyncs && createdCalls == 7, "A persistent mismatch has a bounded recovery burst");
        Scheduler resync; resync.Observe(1, 1, 0);
        Check(!resync.Retry(0), "No resync before the outfit was committed");
        resync.Complete();
        Check(resync.Retry(10) && !resync.Begin(50) && resync.Begin(85) && resync.Resyncs() == 1, "A resync waits like a fresh equip before rebuilding");
        resync.Complete(); resync.Retry(200); resync.Complete(); resync.Retry(300); resync.Complete();
        Check(!resync.Retry(400) && resync.Retry(300 + Scheduler::RecoveryCooldownMs),
            "Persistent failures cool down and can recover later without another equip");
        resync.Complete(); resync.Healthy(40000); resync.Healthy(40000 + Scheduler::HealthyMs);
        Check(!resync.Resyncs() && resync.Retry(44000), "A confirmed healthy outfit restores the recovery budget");
        Check(resync.Observe(1, 2, 50000) && !resync.Resyncs(), "A new outfit restores the resync budget");

        // Repeated abilities must not spend retries on their temporary models,
        // and each later return to human must still restore the persona.
        next = std::make_shared<Snapshot>(*next); ++next->revision;
        next->selections[0] = {49,0,wanted.item,wanted.definition,wanted.style}; Publish(next);
        currentModel = defaultResource.data(); OnThink(hero.data()); Settle(hero.data());
        for (unsigned cycle = 0; cycle < 8; ++cycle) {
            scheduler.Healthy(1); scheduler.Healthy(1 + Scheduler::HealthyMs);
            const auto before = createdCalls;
            currentModel = dragonResource.data(); verifyAt = 0; checkAt = 1; OnThink(hero.data()); Settle(hero.data());
            Check(createdCalls == before && currentModel == dragonResource.data(), "A dragon stays a dragon without human reconstruction");
            currentModel = wearableResource.data(); checkAt = 1; OnThink(hero.data());
            Check(createdCalls == before, "An unfamiliar temporary form is not overwritten");
            currentModel = defaultResource.data(); checkAt = 1; OnThink(hero.data()); Settle(hero.data());
            Check(createdCalls == before + 1 && currentModel == personaResource.data(),
                "Every return to human restores the persona, including beyond three transformations");
        }
        Check(!ModelMatches("default_dragon.vmdl", "default.vmdl") && ModelMatches("default_c_12.vmdl", "default.vmdl"),
            "A shared path prefix is not proof that the base model matches");

        // Reconnect with exactly the same pointer and handle, after the manifest
        // and server equipment were replaced. No new selection is published.
        auto beforeReconnect = createdCalls;
        registeredModels.clear(); currentModel = errorResource.data();
        lastLocalThink = GetTickCount64() - SessionGapMs - 1;
        OnThink(hero.data()); Settle(hero.data());
        Check(createdCalls == beforeReconnect + 1 && currentModel == personaResource.data() && registeredModels.count("dragon.vmdl"),
            "Reconnect with a reused entity re-registers resources and restores the unchanged outfit");
        beforeReconnect = createdCalls;
        NativeVector respawn{}; OnBuildList(hero.data(), reinterpret_cast<void*>(1), nullptr, &respawn);
        currentModel = errorResource.data(); OnThink(hero.data()); Settle(hero.data());
        Check(createdCalls == beforeReconnect + 1 && currentModel == personaResource.data(),
            "A native respawn invalidates completed work even without a long think gap");

        ready = false;
        next = std::make_shared<Snapshot>(*next); ++next->revision; ++next->selections[0].style; ++wanted.style; Publish(next);
        OnThink(hero.data()); Settle(hero.data());
        Check(staged.list.data != nullptr, "Reconnect fixture has an unfinished stage");
        OnThink(otherHero.data());
        Check(staged.list.data != nullptr, "An unrelated hero think cannot cancel the local stage");
        *reinterpret_cast<uint32_t*>(controller.data() + profile::AssignedHero) = 0xffffffff;
        OnThink(hero.data());
        Check(!staged.list.data, "Losing local ownership discards the unfinished stage");
        *reinterpret_cast<uint32_t*>(controller.data() + profile::AssignedHero) = 0x8005;
        ready = true; OnThink(hero.data()); Settle(hero.data());
        Check(rendered.style == wanted.style, "Reacquiring the same hero applies the latest selection");

        beforeReconnect = createdCalls;
        ready = false;
        next = std::make_shared<Snapshot>(*next); ++next->revision; ++next->selections[0].style; ++wanted.style; Publish(next);
        OnThink(hero.data()); Settle(hero.data());
        currentModel = dragonResource.data(); ready = true; OnThink(hero.data()); Settle(hero.data());
        Check(!staged.list.data && createdCalls == beforeReconnect && currentModel == dragonResource.data(),
            "A form change during loading cancels the stale stage without forcing a human model");
        currentModel = defaultResource.data(); Sleep(260); Settle(hero.data());
        Check(createdCalls == beforeReconnect + 1 && rendered.style == wanted.style,
            "An equip deferred during transformation completes after returning to human");

        beforeReconnect = createdCalls;
        failModelLoad = true;
        next = std::make_shared<Snapshot>(*next); ++next->revision; ++next->selections[0].style; ++wanted.style; Publish(next);
        OnThink(hero.data()); Settle(hero.data());
        Check(staged.list.data && createdCalls == beforeReconnect && currentModel == personaResource.data(),
            "An error resource reported ready never replaces the visible outfit");
        staged.started = GetTickCount64() - 10001; OnThink(hero.data());
        Check(!staged.list.data && NativeStatus().phase == Phase::MissingItems,
            "A failed resource stage expires without destroying the previous outfit");
        failModelLoad = false; checkAt = 1; OnThink(hero.data()); Settle(hero.data());
        Check(createdCalls == beforeReconnect + 1 && rendered.style == wanted.style,
            "Transient resource failure recovers without another equip request");
        beforeReconnect = createdCalls;
        *reinterpret_cast<int32_t*>(hero.data() + profile::WearableCount) = 0;
        verifyAt = 0; checkAt = 1; OnThink(hero.data()); Settle(hero.data());
        Check(createdCalls == beforeReconnect + 1 && currentModel == personaResource.data(),
            "Missing wearables recover even when the hero model still matches");

        // Exercise the production path with several identities, without a DK
        // branch in either the lookup fixture or the appearance implementation.
        for (uint32_t heroId : {1u, 55u, 74u, 126u}) {
            *reinterpret_cast<uint32_t*>(hero.data() + profile::HeroId) = heroId;
            auto& assigned = *reinterpret_cast<uint32_t*>(controller.data() + profile::AssignedHero);
            assigned += 0x8000;
            *reinterpret_cast<uint32_t*>(identities.data() + 5 * 0x70 + 0x10) = assigned;
            currentModel = defaultResource.data();
            CaptureSpawnModel(hero.data(), reinterpret_cast<void*>(1));
            next = std::make_shared<Snapshot>(*next); ++next->revision; next->selections[0].hero = heroId; Publish(next);
            OnThink(hero.data()); Settle(hero.data());
            const auto before = createdCalls;
            currentModel = defaultResource.data(); verifyAt = 0; checkAt = 1; OnThink(hero.data()); Settle(hero.data());
            Check(createdCalls == before + 1 && currentModel == personaResource.data() && NativeStatus().hero == heroId,
                "The same appearance recovery applies to each local hero identity");
            assigned += 0x8000;
            *reinterpret_cast<uint32_t*>(identities.data() + 5 * 0x70 + 0x10) = assigned;
            currentModel = errorResource.data();
            OnThink(hero.data()); Settle(hero.data());
            Check(currentModel == personaResource.data() && !strcmp(baselineModel.data(), "default.vmdl"),
                "A new entity with an error model reuses that hero's original baseline");
        }

        ReleaseModels(committedModels);
        Check(*reinterpret_cast<LONG*>(wearableResource.data()+0x20) == 0 && *reinterpret_cast<LONG*>(personaResource.data()+0x20) == 0 &&
            *reinterpret_cast<LONG*>(dragonResource.data()+0x20) == 0,
            "Replacing, cancelling and releasing outfits balances all resource references");
        ReleaseCreationList(*reinterpret_cast<NativeVector*>(hero.data() + profile::CreationList));
        ReleaseModifiers(*reinterpret_cast<void**>(hero.data() + profile::Modifiers));
        VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE); base = 0;
        TlsFree(lookupTls); lookupTls = TLS_OUT_OF_INDEXES;
        std::cout << "PASS: " << checks << " native appearance, identity, profile and scheduling checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n'; return 1;
    }
}
