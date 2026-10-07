// Wardrobe — the one window that replaces the batch files.
//
// It answers "is it working" at a glance, does the one thing that matters on a
// normal day in a single click, and keeps the rare maintenance jobs one step
// away instead of scattered across scripts.
//
// Where an effect can be observed, the observation is what gets reported: the
// activation is called verified only because this window then finds the library
// loaded in the game, never because the loader returned zero.
//
// --capture <png> [--page N] draws the window once and saves it, for a review.
// --run-compat starts the compatibility check as a journal job (review aid).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "brand_mark.h"
#include "capture.h"
#include "environment.h"
#include "tasks.h"
#include "theme.h"
#include "update.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace wardrobe;
namespace fs = std::filesystem;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_target = nullptr;
ID3D11ShaderResourceView* g_brand = nullptr;
bool g_occluded = false;

// ------------------------------------------------------------------ device
bool CreateTarget() {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    g_device->CreateRenderTargetView(back, nullptr, &g_target);
    back->Release();
    return g_target != nullptr;
}

void ReleaseTarget() {
    if (g_target) { g_target->Release(); g_target = nullptr; }
}

bool CreateDevice(HWND window) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, wanted, 2,
                                             D3D11_SDK_VERSION, &description, &g_swap, &g_device, &level, &g_context)))
        return false;
    return CreateTarget();
}

// The brand mark is drawn from pixels compiled into the binary, so the window
// needs no file beside it to look like itself.
void CreateBrandTexture() {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = description.Height = brand::MarkSize;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{brand::Mark, UINT(brand::MarkSize * 4), 0};
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(g_device->CreateTexture2D(&description, &data, &texture)) || !texture) return;
    g_device->CreateShaderResourceView(texture, nullptr, &g_brand);
    texture->Release();
}

void ReleaseDevice() {
    if (g_brand) { g_brand->Release(); g_brand = nullptr; }
    ReleaseTarget();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

LRESULT WINAPI Proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, w, l)) return true;
    switch (message) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED && g_device) {
            ReleaseTarget();
            g_swap->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
            CreateTarget();
        }
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {980, 680};
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

// ------------------------------------------------------------------ state
// The machine is inspected off the UI thread: enumerating processes and reading
// the catalog must never cost a frame.
class Watcher {
public:
    Watcher() : worker_([this] { Loop(); }) {}
    ~Watcher() { stop_ = true; worker_.join(); }

    env::Snapshot Current() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }
    void Nudge() { due_ = 0; }

private:
    void Loop() {
        while (!stop_) {
            auto fresh = env::Look();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_ = std::move(fresh);
            }
            due_ = 30;   // roughly a second and a half between looks
            while (!stop_ && due_-- > 0) Sleep(50);
        }
    }
    mutable std::mutex mutex_;
    env::Snapshot snapshot_;
    std::atomic<bool> stop_{false};
    std::atomic<int> due_{0};
    std::thread worker_;
};

// ------------------------------------------------------------------ screens
enum class Page { Home, Maintenance, Journal };

// A copy built from its sources updates by rebuilding; only an installed copy
// looks for newer releases (a test endpoint turns it on anywhere).
bool InstalledCopy() {
    const auto layout = env::Layout::Discover();
    std::error_code ignored;
    char forced[8]{};
    return GetEnvironmentVariableA("WARDROBE_TEST_RELEASES", forced, sizeof(forced)) > 0 ||
           !fs::exists(layout.project / "src" / "app" / "app.cpp", ignored);
}

struct App {
    Watcher watcher;
    env::CompatWatch compat;
    update::Updater updater{InstalledCopy()};
    task::Runner runner;
    Page page = Page::Home;
    bool askRestore = false;
    // Injecting is the one job whose effect this window can measure for itself,
    // so its verdict comes from the game rather than from an exit code.
    bool verifyActivation = false;
};

const char* PhaseSummary(const std::string& line) {
    if (line.find("phase=4") != std::string::npos) return "La tenue a été transmise au moteur du jeu.";
    if (line.find("phase=3") != std::string::npos) return "Chargement de la tenue en cours.";
    if (line.find("phase=5") != std::string::npos) return "Tenue indisponible : réessaie en rééquipant.";
    if (line.find("phase=2") != std::string::npos) return "Prêt : équipe une tenue dans le menu de Dota.";
    if (line.find("phase=1") != std::string::npos) return "client.dll non reconnu : voir Entretien.";
    return nullptr;
}

void OpenFolder(const char* path) {
    CreateDirectoryA("C:\\Temp", nullptr);
    CreateDirectoryA("C:\\Temp\\opencode", nullptr);
    ShellExecuteA(nullptr, "open", path, nullptr, nullptr, SW_SHOWNORMAL);
}

// Python buffers its output whenever stdout is a pipe, which would leave the
// journal empty for the whole of a long job. -u is honoured by every launcher.
std::wstring Python(const fs::path& interpreter, const fs::path& script, const std::wstring& arguments = {}) {
    return task::Quote(interpreter) + L" -u -X utf8=1 " + task::Quote(script) + (arguments.empty() ? L"" : L" " + arguments);
}

// The journal offers the follow-up to these jobs by name; keep them together.
constexpr const char* RegenerateProfile = "Régénérer le profil de Dota";
constexpr const char* UpdateWardrobe = "Mise à jour de Wardrobe";
constexpr const char* Activation = "Activation de Wardrobe";

// Relocate against the installed Dota, then rebuild everything from it. The
// generator still stops on a changed struct offset or vtable slot; the journal
// then offers to record it, which reruns this same chain with --accept-changes.
std::wstring UpdateCommand(const env::Snapshot& state, bool accept) {
    const auto refresh = state.layout.Script(L"refresh_profile.py");
    return L"cmd.exe /s /c \"" + Python(state.python, refresh, accept ? L"--accept-changes" : L"") + L" && " +
           task::Quote(state.layout.project / "build.bat") + L" --no-pause\"";
}

// Only a copy with its sources, Python and a compiler can adapt itself; what is
// missing otherwise is said in words, never as a button that cannot work.
const char* UpdateUnavailable(const env::Snapshot& state) {
    if (!state.projectSources) return "Cette copie ne contient pas le code source.";
    if (!state.pythonPresent) return "Python n'est pas installé sur cet ordinateur.";
    if (!state.compilerPresent) return "Les outils de compilation Visual Studio ne sont pas installés.";
    if (state.layout.Script(L"refresh_profile.py").empty()) return "Le script refresh_profile.py est introuvable.";
    return nullptr;
}

constexpr const char* CatalogUpdate = "Mise à jour du catalogue";

// The catalog is rebuilt by wardrobe_tools.exe from the game's own files: no
// Python. A copy with its sources also refreshes the project's data/, which
// the next build copies next to the binaries.
std::wstring CatalogCommand(const env::Snapshot& state) {
    std::wstring command = task::Quote(state.layout.Binary(L"wardrobe_tools.exe")) + L" catalog-build";
    if (!state.dotaGame.empty()) command += L" --game " + task::Quote(state.dotaGame);
    command += L" " + task::Quote(state.layout.data / "skins_full.json");
    std::error_code ignored;
    const auto project = state.layout.project / "data" / "skins_full.json";
    if (state.projectSources && !fs::equivalent(project.parent_path(), state.layout.data, ignored))
        command += L" " + task::Quote(project);
    return command;
}

// A slim message under the main panel, for something worth doing that is not
// the thing to do right now. Returns true when its button is pressed.
bool Notice(const char* id, ui::Mark mark, const char* glyph, const char* title, const char* text, const char* button,
            bool enabled, float progress = -1.0f) {
    using namespace ui;
    bool pressed = false;
    if (BeginPanel(id, ImVec2(0, 0), ImVec2(22, 18))) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImU32 tint = mark == Mark::Busy ? color::AccentText : MarkColor(mark);
        const ImVec2 chip(at.x + 20, at.y + 20);
        draw->AddCircleFilled(chip, 20, color::Fade(mark == Mark::Busy ? color::Violet : tint, 0.15f), 32);
        IconAt(draw, chip, glyph, 17, tint);
        constexpr float buttonWidth = 210.0f;
        const float textWidth = ImGui::GetContentRegionAvail().x - 56 - (button ? buttonWidth + 24 : 0);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 56);
        ImGui::BeginGroup();
        Label(fonts().strong, metric::TextLead, color::Text, title);
        Spacer(1);
        Wrapped(fonts().text, metric::TextSmall, color::Muted, text, textWidth);
        if (progress >= 0) {
            Spacer(8);
            ProgressBar(id, progress, textWidth, 6);
        }
        ImGui::EndGroup();
        const float bottom = ImGui::GetCursorPosY();
        if (button) {
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - buttonWidth,
                                       ImGui::GetStyle().WindowPadding.y + 2));
            pressed = SecondaryButton(button, glyph, enabled, buttonWidth, 40) && enabled;
        }
        ImGui::SetCursorPosY(ImMax(bottom, ImGui::GetCursorPosY()) - ImGui::GetStyle().ItemSpacing.y);
        ImGui::Dummy(ImVec2(0, 0));
    }
    EndPanel();
    return pressed;
}

void StartActivation(App& app, const env::Snapshot& state) {
    app.runner.Start(Activation,
                     task::Quote(state.layout.Binary(L"map.exe")) + L" " + task::Quote(state.layout.Binary(L"wardrobe_dll.dll")),
                     state.layout.data.parent_path());
    app.verifyActivation = true;
    app.page = Page::Journal;
}

// A screen's title block: an eyebrow, the title, a sentence.
void PageHeader(const char* eyebrow, const char* title, const char* sentence) {
    using namespace ui;
    Eyebrow(eyebrow, color::AccentText);
    Spacer(2);
    Display(title, 30);
    if (sentence) {
        Spacer(2);
        Wrapped(fonts().text, metric::TextBody, color::Muted, sentence, 620);
    }
    Spacer(metric::Section - 6);
}

// ---- Accueil --------------------------------------------------------------
void DrawHome(App& app, const env::Snapshot& state) {
    using namespace ui;
    const float width = ImGui::GetContentRegionAvail().x;
    const auto compat = app.compat.Current();
    const bool incompatible = compat.state == env::CompatState::Incompatible;
    static char incompatibleDetail[512];

    Mark mark = Mark::Bad;
    const char* pill = "Non prêt";
    const char* headline = "Wardrobe n'est pas prêt";
    const char* detail = "";
    if (state.wardrobeActive) {
        mark = Mark::Ok; pill = "Actif";
        headline = "Wardrobe est actif dans Dota 2";
        detail = "Équipe tes objets dans le menu de Dota, puis lance une partie. En jeu, la touche INSERT ouvre le panneau de diagnostic.";
    } else if (!state.BinariesPresent()) {
        mark = Mark::Bad; pill = "Fichiers manquants";
        headline = "Il manque des fichiers";
        detail = "Le chargeur ou la bibliothèque n'ont pas été trouvés. Réinstalle Wardrobe, ou lance « Recompiler » dans Entretien.";
    } else if (!state.catalog.present) {
        mark = Mark::Bad; pill = "Catalogue manquant";
        headline = "Le catalogue des objets est absent";
        detail = "Va dans Entretien et lance « Mettre à jour le catalogue ». Dota 2 doit être installé.";
    } else if (incompatible) {
        mark = Mark::Bad; pill = "Mise à jour requise";
        headline = "Dota 2 a changé";
        if (state.projectSources)
            sprintf_s(incompatibleDetail, "La dernière mise à jour de Dota a modifié ce dont Wardrobe a besoin (%s). "
                      "Un clic relit le jeu et recompile Wardrobe, en deux minutes environ.", compat.failure.c_str());
        else
            sprintf_s(incompatibleDetail, "Cette version de Wardrobe ne fonctionne pas avec le Dota 2 installé (%s). "
                      "Demande la nouvelle version à la personne qui te l'a donnée et installe-la : c'est tout.",
                      compat.failure.c_str());
        detail = incompatibleDetail;
    } else if (!state.DotaRunning()) {
        mark = Mark::Idle; pill = "Dota 2 fermé";
        headline = "Lance Dota 2 pour commencer";
        detail = "Ouvre Dota 2 et attends le menu principal. Cette fenêtre s'en aperçoit toute seule.";
    } else {
        mark = Mark::Ok; pill = "Prêt";
        headline = "Prêt à activer";
        detail = "Dota 2 est ouvert. Un clic et Wardrobe s'installe dans la partie en cours.";
    }

    // ---- hero
    if (BeginPanel("hero", ImVec2(0, 0), ImVec2(30, 28), true)) {
        const float inner = ImGui::GetContentRegionAvail().x;
        const ImVec2 top = ImGui::GetCursorPos();
        StatusOrb(mark, 30);
        ImGui::SetCursorPos(ImVec2(top.x + 92, top.y + 2));
        ImGui::BeginGroup();
        Pill(mark, pill);
        Spacer(4);
        Display(headline, 30);
        Spacer(2);
        Wrapped(fonts().text, metric::TextBody, color::Muted, detail, inner - 100);
        ImGui::EndGroup();
        Spacer(20);

        const bool busy = app.runner.Busy();
        const float actionWidth = inner;
        if (state.wardrobeActive) {
            PrimaryButton("Wardrobe est déjà actif", icon::Check, "Redémarre Dota 2 pour charger une autre version", false, actionWidth);
        } else if (incompatible && state.BinariesPresent() && state.catalog.present) {
            const char* unavailable = UpdateUnavailable(state);
            if (PrimaryButton("Mettre à jour Wardrobe", icon::Sync,
                              unavailable ? (state.projectSources ? unavailable : "Une nouvelle version de Wardrobe est nécessaire")
                                          : "Relit le jeu installé et recompile Wardrobe",
                              !unavailable && !busy, actionWidth)) {
                app.runner.Start(UpdateWardrobe, UpdateCommand(state, false), state.layout.project);
                app.verifyActivation = false;
                app.page = Page::Journal;
            }
            // The check could be wrong; trying costs nothing and the game says what it found.
            if (state.ReadyToInject()) {
                Spacer(8);
                if (SecondaryButton("Activer quand même", icon::Lightning, !busy)) StartActivation(app, state);
            }
        } else if (!state.DotaRunning() && state.BinariesPresent() && state.catalog.present) {
            if (PrimaryButton("Lancer Dota 2", icon::Play, "Wardrobe pourra s'activer dès que le jeu sera ouvert", true, actionWidth))
                ShellExecuteW(nullptr, L"open", L"steam://rungameid/570", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (PrimaryButton("Activer Wardrobe", icon::Lightning,
                                 state.ReadyToInject() ? "Installe Wardrobe dans la partie en cours" : "Corrige d'abord les points signalés ci-dessous",
                                 state.ReadyToInject() && !busy, actionWidth)) {
            StartActivation(app, state);
        }
    }
    EndPanel();

    // ---- things worth doing, that are not the thing to do
    {
        const auto release = app.updater.Current();
        static char text[512];
        if (release.state == update::State::Available || release.state == update::State::Downloading ||
            release.state == update::State::Ready || (release.state == update::State::Failed && !release.url.empty())) {
            Spacer(metric::Gap);
            const bool downloading = release.state == update::State::Downloading || release.state == update::State::Ready;
            if (release.state == update::State::Failed)
                sprintf_s(text, "Le téléchargement de la version %s a échoué (%s). Réessaie dans un instant.", release.version.c_str(), release.error.c_str());
            else if (downloading)
                sprintf_s(text, "Téléchargement de la version %s… Wardrobe se fermera puis se rouvrira tout seul.", release.version.c_str());
            else
                sprintf_s(text, "La version %s est disponible. Un clic la télécharge et l'installe ; Wardrobe redémarre tout seul.",
                          release.version.c_str());
            if (Notice("maj-app", Mark::Busy, icon::Download, "Nouvelle version de Wardrobe", text,
                       downloading ? u8"Téléchargement…" : "Mettre à jour", !downloading, downloading ? release.progress : -1.0f))
                app.updater.Download();
        }
        const auto fresh = app.compat.Catalog();
        if (fresh.state == env::FreshState::Outdated && state.catalog.present) {
            Spacer(metric::Gap);
            std::string names;
            for (size_t i = 0; i < fresh.examples.size(); ++i) names += (i ? ", " : "") + fresh.examples[i];
            sprintf_s(text, "%zu cosmétique%s de la dernière mise à jour de Dota manque%s au catalogue (dont %s). Un clic les ajoute, en quelques secondes.",
                      fresh.missing, fresh.missing > 1 ? "s" : "", fresh.missing > 1 ? "nt" : "", names.c_str());
            const bool possible = !state.layout.Binary(L"wardrobe_tools.exe").empty() && state.DotaInstalled() && !app.runner.Busy();
            if (Notice("maj-catalogue", Mark::Warn, icon::Library, "De nouveaux cosmétiques sont sortis", text, "Mettre à jour le catalogue", possible)) {
                app.runner.Start(CatalogUpdate, CatalogCommand(state), state.layout.project);
                app.verifyActivation = false;
                app.page = Page::Journal;
            }
        }
    }

    // ---- what the window knows
    Spacer(22);
    Eyebrow("ÉTAT DE L'ORDINATEUR");
    Spacer(4);
    {
        static char catalogue[96];
        const auto fresh = app.compat.Catalog();
        Mark catalogMark = state.catalog.present ? Mark::Ok : Mark::Bad;
        if (!state.catalog.present) strcpy_s(catalogue, "absent");
        else if (fresh.state == env::FreshState::Outdated) {
            sprintf_s(catalogue, "%zu nouveaux objets à ajouter", fresh.missing);
            catalogMark = Mark::Warn;
        } else if (fresh.state == env::FreshState::UpToDate) sprintf_s(catalogue, "%zu objets · à jour", state.catalog.entries);
        else sprintf_s(catalogue, "%zu objets · %s", state.catalog.entries, state.catalog.updated.c_str());
        Mark compatMark = Mark::Busy;
        const char* compatValue = u8"vérification…";
        switch (compat.state) {
        case env::CompatState::Compatible: compatMark = Mark::Ok; compatValue = compat.exact ? "oui" : "oui, adapté seul à la mise à jour"; break;
        case env::CompatState::Incompatible: compatMark = Mark::Bad; compatValue = "non, mise à jour requise"; break;
        case env::CompatState::NoDota: compatMark = Mark::Idle; compatValue = "Dota 2 introuvable"; break;
        case env::CompatState::NoTool: compatMark = Mark::Idle; compatValue = "vérification indisponible"; break;
        default: break;
        }
        struct Fact { const char* id; const char* glyph; const char* label; const char* value; Mark mark; };
        const Fact facts[] = {
            {"dota", icon::Game, "Dota 2", state.DotaRunning() ? "en cours d'exécution" : "fermé", state.DotaRunning() ? Mark::Ok : Mark::Idle},
            {"compat", icon::Shield, "Compatible avec cette version de Dota 2", compatValue, compatMark},
            {"install", icon::Folder, "Installation de Dota 2", state.DotaInstalled() ? "trouvée" : "introuvable", state.DotaInstalled() ? Mark::Ok : Mark::Bad},
            {"catalog", icon::Library, "Catalogue des objets", catalogue, catalogMark},
            {"files", icon::Package, "Chargeur et bibliothèque", state.BinariesPresent() ? "présents" : "manquants", state.BinariesPresent() ? Mark::Ok : Mark::Bad},
            // Python is never needed to use Wardrobe; its absence is a fact, not a fault.
            {"python", icon::Code, "Python, pour l'entretien", state.pythonPresent ? "installé" : "absent, non requis", state.pythonPresent ? Mark::Ok : Mark::Idle},
        };
        const float column = (width - metric::Gap) * 0.5f;
        for (size_t i = 0; i < std::size(facts); ++i) {
            if (i % 2) ImGui::SameLine(0, metric::Gap);
            CheckTile(facts[i].id, facts[i].glyph, facts[i].label, facts[i].value, facts[i].mark, column);
        }
    }

    // Only while the library is loaded is its log a live account of the game;
    // an older line would describe a session that has nothing to do with now.
    if (state.wardrobeActive) {
        if (const char* summary = PhaseSummary(state.appearanceLine)) {
            Spacer(metric::Section);
            Eyebrow("EN JEU");
            Spacer(4);
            if (BeginPanel("rapport", ImVec2(0, 0), ImVec2(22, 18))) {
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const ImVec2 at = ImGui::GetCursorScreenPos();
                IconAt(draw, ImVec2(at.x + 10, at.y + 10), icon::Game, 16, color::AccentText);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 32);
                Body(color::Text, summary);
            }
            EndPanel();
        }
    }

    // ---- how it works
    Spacer(22);
    Eyebrow("COMMENT ÇA MARCHE");
    Spacer(4);
    const float step = (width - metric::Gap * 2) / 3.0f;
    StepCard("s1", 1, "Ouvre Dota 2", "Attends le menu principal. Wardrobe le voit tout seul.", step, 104);
    ImGui::SameLine(0, metric::Gap);
    StepCard("s2", 2, "Active Wardrobe", "Un clic sur le bouton ci-dessus, pendant que le jeu est ouvert.", step, 104);
    ImGui::SameLine(0, metric::Gap);
    StepCard("s3", 3, "Équipe et joue", "Choisis tes objets dans le menu de Dota, puis lance une partie.", step, 104);
    Spacer(metric::Pad);
}

// ---- Entretien ------------------------------------------------------------
struct Job {
    const char* id;
    const char* glyph;
    const char* title;
    const char* description;
    const char* button;
    const char* unavailable;      // why it cannot run now, or nullptr
    std::wstring commandLine;
    fs::path workingDirectory;
};

// One row per job: an icon, what it does, why it cannot run if it cannot, and
// its button. Returns true when the button was pressed (and the job may run).
bool JobRow(App& app, const Job& job, bool custom = false) {
    using namespace ui;
    bool pressed = false;
    if (BeginPanel(job.id, ImVec2(0, 0), ImVec2(22, 20))) {
        const bool available = !job.unavailable && !app.runner.Busy();
        constexpr float buttonWidth = 168.0f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 chip(at.x + 20, at.y + 20);
        draw->AddRectFilled(ImVec2(chip.x - 20, chip.y - 20), ImVec2(chip.x + 20, chip.y + 20),
                            color::Fade(job.unavailable ? color::Faint : color::Violet, 0.14f), 11);
        IconAt(draw, chip, job.glyph, 17, job.unavailable ? color::Faint : color::AccentText);
        const float right = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 56);
        ImGui::BeginGroup();
        Label(fonts().strong, metric::TextLead, color::Text, job.title);
        Spacer(1);
        Wrapped(fonts().text, metric::TextSmall, color::Muted, job.description, right - 56 - buttonWidth - 24);
        if (job.unavailable) {
            Spacer(3);
            Wrapped(fonts().text, metric::TextSmall, color::Warn, job.unavailable, right - 56 - buttonWidth - 24);
        }
        ImGui::EndGroup();
        const float bottom = ImGui::GetCursorPosY();
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - buttonWidth,
                                   ImGui::GetStyle().WindowPadding.y + 2));
        if (SecondaryButton(job.button, nullptr, available, buttonWidth) && available) {
            pressed = true;
            if (!custom) {
                app.runner.Start(job.title, job.commandLine, job.workingDirectory);
                app.verifyActivation = false;
                app.page = Page::Journal;
            }
        }
        // The row ends where its taller column ends, without an extra line of spacing.
        ImGui::SetCursorPosY(ImMax(bottom, ImGui::GetCursorPosY()) - ImGui::GetStyle().ItemSpacing.y);
        ImGui::Dummy(ImVec2(0, 0));
    }
    EndPanel();
    Spacer(metric::Gap);
    return pressed;
}

void DrawMaintenance(App& app, const env::Snapshot& state) {
    using namespace ui;
    PageHeader("ENTRETIEN", "Entretien",
               "Utile seulement après une mise à jour de Dota 2, ou si l'accueil signale un problème. Rien ici n'est nécessaire au quotidien.");

    const auto python = state.python;
    const char* noPython = state.pythonPresent ? nullptr : "Python n'est pas installé sur cet ordinateur.";
    const auto refresh = state.layout.Script(L"refresh_profile.py");
    const auto update = state.layout.Script(L"update_db.py");
    const auto tools = state.layout.Binary(L"wardrobe_tools.exe");

    // The same check as the home screen, run by the DLL's own resolver: no Python.
    JobRow(app, {"j-compat", icon::Shield, "Vérifier la compatibilité avec Dota",
                 "Vérifie que cette version de Wardrobe retrouve ce dont elle a besoin dans le Dota 2 installé. N'écrit rien. Quelques secondes.",
                 "Vérifier", tools.empty() ? "L'outil wardrobe_tools.exe est introuvable." : nullptr,
                 task::Quote(tools) + L" compat", state.layout.project});

    // Rebuilt by wardrobe_tools.exe from the game's files, exactly like update_db.py: no Python.
    JobRow(app, {"j-catalog", icon::Download, "Mettre à jour le catalogue",
                 "Après une mise à jour de contenu de Dota 2 : relit les objets dans les fichiers du jeu et reconstruit le catalogue. Quelques secondes.",
                 "Mettre à jour",
                 tools.empty() ? "L'outil wardrobe_tools.exe est introuvable." : !state.DotaInstalled() ? "Installation de Dota 2 introuvable." : nullptr,
                 CatalogCommand(state), state.layout.project});
    (void)update;

    JobRow(app, {"j-profile", icon::Sync, RegenerateProfile,
                 "Seulement si la vérification signale un problème. Le résultat ne prend effet qu'après une recompilation. Environ une minute.",
                 "Régénérer",
                 !state.projectSources ? "Indisponible dans cette copie : elle ne contient pas le code source. Demande une version à jour à la personne qui t'a donné Wardrobe."
                 : refresh.empty() ? "Le script est introuvable." : noPython,
                 Python(python, refresh), state.layout.project});

    // Only shown where there is something to compile; a shared copy has binaries only.
    if (state.projectSources)
        JobRow(app, {"j-build", icon::Code, "Recompiler Wardrobe",
                     "Reconstruit l'application, le chargeur, la bibliothèque et l'outil à partir du code source. Nécessite Visual Studio.",
                     "Recompiler", state.compilerPresent ? nullptr : "Les outils de compilation Visual Studio ne sont pas installés.",
                     L"cmd.exe /c " + task::Quote(state.layout.project / "build.bat") + L" --no-pause", state.layout.project});

    const char* restoreBlocked = !state.DotaInstalled() ? "Installation de Dota 2 introuvable."
                                 : state.DotaRunning() ? "Ferme complètement Dota 2 avant de continuer."
                                 : tools.empty()       ? "L'outil wardrobe_tools.exe est introuvable." : nullptr;
    if (JobRow(app, {"j-restore", icon::Restore, "Restaurer l'inventaire Steam",
                     "Dota 2 peut garder des objets d'aperçu et les afficher même sans Wardrobe. Met de côté les fichiers concernés pour que Steam les retélécharge.",
                     u8"Restaurer…", restoreBlocked, {}, {}}, true))
        app.askRestore = true;
}

// ---- Journal --------------------------------------------------------------
void DrawJournal(App& app, const env::Snapshot& world) {
    using namespace ui;
    const auto state = app.runner.CurrentState();
    const std::string title = app.runner.Title();
    const auto output = app.runner.Output();

    if (state == task::State::Idle && output.empty()) {
        PageHeader("JOURNAL", "Journal", nullptr);
        if (BeginPanel("vide", ImVec2(0, 280), ImVec2(30, 30), true)) {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 min = ImGui::GetWindowPos();
            const float w = ImGui::GetWindowWidth();
            const ImVec2 centre(min.x + w * 0.5f, min.y + 96);
            Glow(draw, centre, 90, color::Fade(color::Violet, 0.16f));
            draw->AddCircleFilled(centre, 34, color::Fade(color::Violet, 0.14f), 48);
            draw->AddCircle(centre, 34, color::Fade(color::AccentText, 0.35f), 48);
            IconAt(draw, centre, icon::Journal, 26, color::AccentText);
            ImGui::SetCursorPosY(150);
            const char* line1 = "Rien n'a encore été lancé";
            ImGui::PushFont(fonts().strong, metric::TextLead);
            ImGui::SetCursorPosX((w - ImGui::CalcTextSize(line1).x) * 0.5f);
            ImGui::PopFont();
            Label(fonts().strong, metric::TextLead, color::Text, line1);
            const char* line2 = "Chaque activation et chaque opération d'entretien écrit ici ce qu'elle a vraiment fait.";
            ImGui::PushFont(fonts().text, metric::TextSmall);
            ImGui::SetCursorPosX((w - ImGui::CalcTextSize(line2).x) * 0.5f);
            ImGui::PopFont();
            Small(color::Muted, line2);
            const char* line3 = "Le jeu tient son propre journal, dans C:\\Temp\\opencode.";
            ImGui::PushFont(fonts().text, metric::TextSmall);
            ImGui::SetCursorPosX((w - ImGui::CalcTextSize(line3).x) * 0.5f);
            ImGui::PopFont();
            Small(color::Faint, line3);
        }
        EndPanel();
        Spacer(metric::Pad);
        if (SecondaryButton("Ouvrir le dossier des journaux", icon::Folder, true)) OpenFolder("C:\\Temp\\opencode");
        return;
    }

    // An exit code says the tool ran, not that the world changed. Where the
    // change is observable from here, the observation is what gets reported.
    const bool measured = app.verifyActivation && state == task::State::Succeeded;
    const bool active = world.wardrobeActive;
    const Mark mark = state == task::State::Running ? Mark::Busy
                      : state == task::State::Failed ? Mark::Bad
                      : measured && !active        ? Mark::Warn
                                                   : Mark::Ok;
    const char* label = state == task::State::Running ? "En cours"
                        : state == task::State::Failed ? "Échec"
                        : measured && active   ? "Vérifié"
                        : measured && !active  ? "Sans effet"
                                               : "Terminé";
    const char* verdict = nullptr;
    ImU32 verdictTint = color::Muted;
    if (state == task::State::Failed) {
        verdict = "L'opération s'est arrêtée sur une erreur. Le détail est ci-dessous ; rien n'a été modifié au-delà de ce qui y est écrit.";
        verdictTint = color::Bad;
    } else if (measured && active) {
        verdict = "Wardrobe est bien chargé dans Dota 2 : cette fenêtre l'a constaté elle-même. Équipe maintenant tes objets dans le menu de Dota.";
        verdictTint = color::Ok;
    } else if (measured && !active) {
        verdict = "Le chargeur s'est terminé sans erreur, mais Wardrobe n'apparaît pas dans Dota 2. Ferme le jeu, rouvre-le, et réessaie.";
        verdictTint = color::Warn;
    } else if (state == task::State::Succeeded && (title == CatalogUpdate || title == "Mettre à jour le catalogue")) {
        verdict = "Le catalogue est à jour. Redémarre Dota 2 puis réactive Wardrobe : les nouveaux objets apparaîtront dans le menu de Dota.";
        verdictTint = color::Ok;
    } else if (state == task::State::Succeeded && title == UpdateWardrobe) {
        verdict = "Wardrobe a été relu contre le Dota installé et recompilé. L'accueil revérifie tout seul. Si Dota 2 est ouvert, redémarre-le pour qu'il charge la nouvelle version.";
        verdictTint = color::Ok;
    } else if (state == task::State::Succeeded) {
        verdict = "L'outil s'est terminé sans erreur. Ce qu'il a fait est écrit ci-dessous.";
    } else if (output.size() <= 1) {
        verdict = "L'outil vient de démarrer. Certaines opérations restent silencieuses une quinzaine de secondes avant d'écrire leur première ligne.";
    }

    // ---- header: what ran, how it ended
    {
        Eyebrow("JOURNAL", color::AccentText);
        Spacer(2);
        Display(title.c_str(), 30);
        Spacer(4);
        Pill(mark, label);
        ImGui::SameLine(0, 12);
        char elapsed[48];
        sprintf_s(elapsed, "%.0f s", app.runner.Seconds());
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
        Label(fonts().text, metric::TextSmall, color::Faint, elapsed);
        if (verdict) {
            Spacer(6);
            Wrapped(fonts().text, metric::TextBody, verdictTint, verdict, 700);
        }
        Spacer(metric::Pad);
    }

    // Regenerating stops rather than record a value that moved, because only a
    // person can tell a real Dota change from a mis-picked anchor. That person is
    // reading the lines just below, so the answer belongs here and not on a flag
    // that only a console can pass.
    const bool awaitingAccept =
        state == task::State::Failed && (title == RegenerateProfile || title == UpdateWardrobe) &&
        std::any_of(output.begin(), output.end(),
                    [](const std::string& line) { return line.find("--accept-changes") != std::string::npos; });
    if (awaitingAccept) {
        const auto script = world.layout.Script(L"refresh_profile.py");
        const bool possible = world.pythonPresent && world.projectSources && !script.empty() && !app.runner.Busy();
        Job accept{"accepter", icon::Check, "Enregistrer ces valeurs",
                   "Des valeurs notées par Wardrobe ne sont plus les mêmes dans cette version de Dota 2 (détail ci-dessous). "
                   "Si ces lignes ne parlent que de « global », c'est ce que fait chaque mise à jour du jeu.",
                   "Enregistrer", possible ? nullptr : "Impossible ici : il manque Python ou le code source du projet.", {}, {}};
        if (JobRow(app, accept, true)) {
            if (title == UpdateWardrobe) app.runner.Start(UpdateWardrobe, UpdateCommand(world, true), world.layout.project);
            else app.runner.Start(RegenerateProfile, Python(world.python, script, L"--accept-changes"), world.layout.project);
            app.verifyActivation = false;
        }
    }

    // ---- console
    const float footer = 56;
    const float height = ImMax(160.0f, ImGui::GetContentRegionAvail().y - footer);
    if (BeginPanel("console", ImVec2(0, height), ImVec2(0, 0))) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 min = ImGui::GetWindowPos();
        const float w = ImGui::GetWindowWidth();
        // a slim title bar: three quiet dots and the word "sortie"
        draw->AddRectFilled(min, ImVec2(min.x + w, min.y + 36), color::Fade(color::White, 0.02f), metric::CardRadius, ImDrawFlags_RoundCornersTop);
        draw->AddLine(ImVec2(min.x, min.y + 36), ImVec2(min.x + w, min.y + 36), color::Hairline);
        for (int i = 0; i < 3; ++i) draw->AddCircleFilled(ImVec2(min.x + 20 + i * 14, min.y + 18), 4, color::Raised, 12);
        draw->AddText(fonts().strong, metric::TextTiny, ImVec2(min.x + 68, min.y + 11), color::Faint, "SORTIE");
        if (app.runner.Busy()) SpinnerAt(draw, ImVec2(min.x + w - 22, min.y + 18), 6, color::Azure, 1.8f);
        ImGui::SetCursorPos(ImVec2(0, 37));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 14));
        ImGui::BeginChild("lignes", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushFont(fonts().mono, metric::TextSmall);
        for (const auto& line : output) {
            ImU32 tint = color::Muted;
            if (line.rfind("> ", 0) == 0) tint = color::AccentText;
            else if (line.find("[!]") != std::string::npos || line.find("error") != std::string::npos ||
                     line.find("FAIL") != std::string::npos) tint = color::Bad;
            else if (line.find("[OK]") != std::string::npos || line.find("PASS") != std::string::npos) tint = color::Ok;
            else if (line.rfind("[*]", 0) == 0) tint = color::Text;
            ImGui::PushStyleColor(ImGuiCol_Text, tint);
            ImGui::TextWrapped("%s", line.c_str());
            ImGui::PopStyleColor();
        }
        if (app.runner.Busy()) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    EndPanel();

    Spacer(12);
    if (SecondaryButton("Ouvrir le dossier des journaux", icon::Folder, true)) OpenFolder("C:\\Temp\\opencode");
    if (state == task::State::Succeeded || state == task::State::Failed) {
        ImGui::SameLine(0, metric::Gap);
        if (SecondaryButton("Effacer", icon::Erase, true)) app.runner.Acknowledge();
    }
}

// ---- chrome ---------------------------------------------------------------
void DrawRail(App& app, const env::Snapshot& state) {
    using namespace ui;
    constexpr float railWidth = 252;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 26));
    ImGui::BeginChild("rail", ImVec2(railWidth, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetWindowPos();
    const float height = ImGui::GetWindowHeight();
    draw->AddRectFilled(min, ImVec2(min.x + railWidth, min.y + height), color::Fade(color::Surface, 0.55f));
    draw->AddLine(ImVec2(min.x + railWidth - 1, min.y), ImVec2(min.x + railWidth - 1, min.y + height), color::Hairline);

    // brand
    {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        constexpr float mark = 40;
        if (g_brand) {
            draw->PushClipRect(ImVec2(at.x - 30, at.y - 30), ImVec2(at.x + 80, at.y + 80), false);
            Glow(draw, ImVec2(at.x + 6 + mark * 0.5f, at.y + mark * 0.5f), 46, color::Fade(color::Violet, 0.25f));
            draw->PopClipRect();
            draw->AddImageRounded(ImTextureRef(ImTextureID(g_brand)), ImVec2(at.x + 6, at.y), ImVec2(at.x + 6 + mark, at.y + mark),
                                  ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 10);
        }
        draw->AddText(fonts().display, 24, ImVec2(at.x + 58, at.y - 1), color::Text, "Wardrobe");
        draw->AddText(fonts().text, metric::TextTiny, ImVec2(at.x + 59, at.y + 25), color::Faint, u8"Cosmétiques Dota 2");
        ImGui::Dummy(ImVec2(railWidth - 32, mark));
    }
    Spacer(30);
    Eyebrow("  NAVIGATION");
    Spacer(4);

    const float width = ImGui::GetContentRegionAvail().x;
    const auto job = app.runner.CurrentState();
    const Mark jobMark = app.runner.Busy() ? Mark::Busy : job == task::State::Succeeded ? Mark::Ok : Mark::Bad;
    if (NavItem(icon::Home, "Accueil", app.page == Page::Home, Mark::Idle, false, width)) app.page = Page::Home;
    if (NavItem(icon::Tools, "Entretien", app.page == Page::Maintenance, Mark::Idle, false, width)) app.page = Page::Maintenance;
    // A job that ended while the user was elsewhere says so on the tab.
    if (NavItem(icon::Journal, "Journal", app.page == Page::Journal, jobMark, job != task::State::Idle, width)) app.page = Page::Journal;

    // live status, at the foot of the rail
    ImGui::SetCursorPosY(height - 26 - 112);
    if (BeginPanel("pied", ImVec2(width, 112), ImVec2(16, 14))) {
        const Mark mark = state.wardrobeActive ? Mark::Ok : state.DotaRunning() ? Mark::Busy : Mark::Idle;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImDrawList* panel = ImGui::GetWindowDrawList();
        const ImVec2 dot(at.x + 5, at.y + 9);
        if (mark == Mark::Ok) Glow(panel, dot, 12, color::Fade(color::Ok, 0.45f));
        panel->AddCircleFilled(dot, 4.5f, mark == Mark::Busy ? color::Azure : MarkColor(mark), 16);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 18);
        Label(fonts().strong, metric::TextSmall, color::Text,
              state.wardrobeActive ? "Actif dans Dota 2" : state.DotaRunning() ? "Dota 2 ouvert" : u8"Dota 2 fermé");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 16 - ImGui::CalcTextSize("v" WARDROBE_VERSION).x * (metric::TextTiny / metric::TextBody));
        Label(fonts().text, metric::TextTiny, color::Faint, "v" WARDROBE_VERSION);
        Spacer(8);
        if (SecondaryButton(u8"Revérifier", icon::Refresh, true, ImGui::GetContentRegionAvail().x, 36)) {
            app.watcher.Nudge();
            app.compat.Nudge();
        }
    }
    EndPanel();

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void DrawConfirm(App& app, const env::Snapshot& state) {
    using namespace ui;
    const char* name = "Restaurer l'inventaire Steam";
    if (app.askRestore) { ImGui::OpenPopup(name); app.askRestore = false; }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(540, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30, 28));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, color::Vec(color::Surface));
    if (ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 chip(at.x + 22, at.y + 22);
        draw->AddCircleFilled(chip, 22, color::Fade(color::Warn, 0.14f), 32);
        IconAt(draw, chip, icon::Restore, 19, color::Warn);
        ImGui::Dummy(ImVec2(44, 44));
        Spacer(10);
        Title(name);
        Spacer(4);
        Body(color::Muted, "Les fichiers de cache de Dota 2 qui contiennent des objets d'aperçu vont être déplacés dans une sauvegarde, "
                           "puis retirés du jeu. Steam les reconstruira à la prochaine connexion.");
        Spacer(6);
        Small(color::Faint, "Une copie vérifiée est gardée dans %LOCALAPPDATA%\\Wardrobe\\recovery ; rien n'est supprimé définitivement. "
                            "Les fichiers sans objet d'aperçu ne sont pas touchés.");
        Spacer(22);
        const float half = (ImGui::GetContentRegionAvail().x - metric::Gap) * 0.5f;
        if (SecondaryButton("Annuler", nullptr, true, half, 48)) ImGui::CloseCurrentPopup();
        ImGui::SameLine(0, metric::Gap);
        if (PrimaryButton("Restaurer", icon::Restore, nullptr, true, half)) {
            app.runner.Start(name,
                             task::Quote(state.layout.Binary(L"wardrobe_tools.exe")) + L" repair " + task::Quote(state.dotaGame) + L" --repair",
                             state.layout.project);
            app.verifyActivation = false;
            app.page = Page::Journal;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void DrawFrame(App& app) {
    using namespace ui;
    const auto state = app.watcher.Current();
    static task::State previous = task::State::Idle;
    const auto now = app.runner.CurrentState();
    if (previous == task::State::Running && now != task::State::Running) { app.watcher.Nudge(); app.compat.Nudge(); }
    previous = now;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    Backdrop(ImGui::GetBackgroundDrawList(), viewport->Pos,
             ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##wardrobe", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();

    DrawRail(app, state);
    ImGui::SameLine(0, 0);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36, 30));
    ImGui::BeginChild("contenu", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    const float entering = PageEnter(int(app.page));
    switch (app.page) {
    case Page::Home: DrawHome(app, state); break;
    case Page::Maintenance: DrawMaintenance(app, state); break;
    case Page::Journal: DrawJournal(app, state); break;
    }
    PageExit(entering);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    DrawConfirm(app, state);
    ImGui::End();
}

// ------------------------------------------------------------------ capture
// Used by --capture so the interface can be reviewed as an image.
bool SaveFrame(const char* path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    D3D11_TEXTURE2D_DESC description{};
    back->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_device->CreateTexture2D(&description, nullptr, &staging))) { back->Release(); return false; }
    g_context->CopyResource(staging, back);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    bool ok = false;
    if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        const uint32_t width = description.Width, height = description.Height;
        std::vector<uint8_t> rows(size_t(width) * height * 3);
        for (uint32_t y = 0; y < height; ++y) {
            const auto* source = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
            uint8_t* destination = rows.data() + size_t(y) * width * 3;
            for (uint32_t x = 0; x < width; ++x) {
                destination[x * 3 + 0] = source[x * 4 + 0];
                destination[x * 3 + 1] = source[x * 4 + 1];
                destination[x * 3 + 2] = source[x * 4 + 2];
            }
        }
        g_context->Unmap(staging, 0);
        ok = capture::WritePng(path, rows.data(), width, height);
    }
    staging->Release();
    back->Release();
    return ok;
}

// Windows 11 draws the caption in the app's own colour and rounds the corners;
// Windows 10 ignores the attributes it does not know.
void StyleWindowFrame(HWND window) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = RGB(0x0D, 0x0F, 0x15), text = RGB(0xC9, 0xCF, 0xDC), border = RGB(0x1E, 0x22, 0x2C);
    DwmSetWindowAttribute(window, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
    DwmSetWindowAttribute(window, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    const int round = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring capture;
    int capturePage = 0;
    bool runCompat = false;
    if (int count = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) {
            if (!wcscmp(argv[i], L"--capture") && i + 1 < count) capture = argv[++i];
            else if (!wcscmp(argv[i], L"--page") && i + 1 < count) capturePage = _wtoi(argv[++i]);
            else if (!wcscmp(argv[i], L"--run-compat")) runCompat = true;
        }
        LocalFree(argv);
    }

    WNDCLASSEXW window{sizeof(window), CS_CLASSDC, Proc, 0, 0, instance, LoadIconW(instance, MAKEINTRESOURCEW(1)), nullptr,
                       nullptr, nullptr, L"WardrobeApp", LoadIconW(instance, MAKEINTRESOURCEW(1))};
    RegisterClassExW(&window);
    HWND handle = CreateWindowW(window.lpszClassName, L"Wardrobe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                1180, 860, nullptr, nullptr, instance, nullptr);
    if (!CreateDevice(handle)) {
        ReleaseDevice();
        UnregisterClassW(window.lpszClassName, instance);
        MessageBoxW(nullptr, L"Impossible d'initialiser l'affichage (DirectX 11).", L"Wardrobe", MB_ICONERROR);
        return 1;
    }
    CreateBrandTexture();
    StyleWindowFrame(handle);
    ShowWindow(handle, capture.empty() ? SW_SHOWDEFAULT : SW_SHOWNA);
    UpdateWindow(handle);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ui::LoadFonts();
    ui::ApplyStyle();
    ImGui_ImplWin32_Init(handle);
    ImGui_ImplDX11_Init(g_device, g_context);

    App app;
    if (capturePage) app.page = Page(capturePage);
    int frames = 0;
    bool running = true;
    while (running) {
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) { Sleep(16); continue; }
        g_occluded = false;

        if (runCompat && frames == 2) {
            const auto layout = env::Layout::Discover();
            app.runner.Start("Vérifier la compatibilité avec Dota", task::Quote(layout.Binary(L"wardrobe_tools.exe")) + L" compat",
                             layout.project);
            app.page = Page::Journal;
        }

        if (capture.empty()) {
            const auto release = app.updater.Current();
            if (release.state == update::State::Ready) {
                const std::wstring arguments = L"--update --target " + task::Quote(env::Layout::Discover().exeDirectory);
                ShellExecuteW(nullptr, L"open", release.installer.c_str(), arguments.c_str(), nullptr, SW_SHOWNORMAL);
                running = false;
                break;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawFrame(app);
        ImGui::Render();
        const float clear[4] = {0.035f, 0.043f, 0.063f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // A capture shows settled state: the compatibility verdict, a finished
        // job and a page that has finished arriving (10 s at most).
        const auto verdict = app.compat.Current().state;
        const bool settled = verdict != env::CompatState::Unknown && verdict != env::CompatState::Checking &&
                             !app.runner.Busy() && (!runCompat || frames > 4);
        if (!capture.empty() && ++frames > 40 && (settled || frames > 600)) {
            char path[MAX_PATH]{};
            WideCharToMultiByte(CP_UTF8, 0, capture.c_str(), -1, path, MAX_PATH, nullptr, nullptr);
            SaveFrame(path);
            running = false;
        } else if (capture.empty()) {
            ++frames;
        }
        g_occluded = g_swap->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ReleaseDevice();
    DestroyWindow(handle);
    UnregisterClassW(window.lpszClassName, instance);
    return 0;
}
