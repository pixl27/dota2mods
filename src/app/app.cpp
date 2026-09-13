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
// --capture <png> [--page N] draws the window once and saves it, for a report.
#define WIN32_LEAN_AND_MEAN
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
#include "capture.h"
#include "environment.h"
#include "tasks.h"
#include "theme.h"

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

void ReleaseDevice() {
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
        reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {900, 620};
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

struct App {
    Watcher watcher;
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

// ---- Accueil --------------------------------------------------------------
void DrawHome(App& app, const env::Snapshot& state) {
    using namespace ui;
    const float width = ImGui::GetContentRegionAvail().x;

    if (BeginCard("etat")) {
        Mark mark = Mark::Bad;
        const char* pill = "Non prêt";
        const char* headline = "Wardrobe n'est pas prêt";
        const char* detail = "";
        if (state.wardrobeActive) {
            mark = Mark::Ok; pill = "Actif";
            headline = "Wardrobe est actif dans Dota 2";
            detail = "Équipe tes objets dans le menu de Dota, puis lance une partie. En jeu, INSERT ouvre le panneau de diagnostic.";
        } else if (!state.BinariesPresent()) {
            mark = Mark::Bad; pill = "Fichiers manquants";
            headline = "Il manque des fichiers";
            detail = "Le chargeur ou la bibliothèque n'ont pas été trouvés. Va dans Entretien et lance « Recompiler ».";
        } else if (!state.catalog.present) {
            mark = Mark::Bad; pill = "Catalogue manquant";
            headline = "Le catalogue des objets est absent";
            detail = "Va dans Entretien et lance « Mettre à jour le catalogue ». Dota 2 doit être installé.";
        } else if (!state.DotaRunning()) {
            mark = Mark::Warn; pill = "Dota 2 fermé";
            headline = "Lance Dota 2 pour commencer";
            detail = "Ouvre Dota 2 et attends le menu principal. Cette fenêtre s'en apercevra toute seule.";
        } else {
            mark = Mark::Ok; pill = "Prêt";
            headline = "Prêt à activer";
            detail = "Dota 2 est ouvert. Un clic et Wardrobe s'installe dans la partie en cours.";
        }
        Pill(mark, pill);
        Spacer(6);
        Sized(fonts().strong, metric::TextDisplay, color::Text, headline);
        Spacer(2);
        SmallWrapped(color::Muted, detail);
        Spacer(metric::Pad);

        const bool busy = app.runner.Busy();
        const float actionWidth = width - 40;
        if (state.wardrobeActive) {
            Action("Wardrobe est déjà actif", "Redémarre Dota 2 pour recharger une autre version", false, actionWidth);
        } else if (!state.DotaRunning() && state.BinariesPresent() && state.catalog.present) {
            if (Action("Lancer Dota 2", "Wardrobe s'activera dès que le jeu sera ouvert", true, actionWidth))
                ShellExecuteW(nullptr, L"open", L"steam://rungameid/570", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (Action("Activer Wardrobe", state.ReadyToInject() ? "Installe Wardrobe dans la partie en cours"
                                                                   : "Corrige d'abord les points signalés ci-dessous",
                          state.ReadyToInject() && !busy, actionWidth)) {
            const auto loader = state.layout.Binary(L"map.exe");
            const auto payload = state.layout.Binary(L"wardrobe_dll.dll");
            app.runner.Start("Activation de Wardrobe",
                             task::Quote(loader) + L" " + task::Quote(payload),
                             state.layout.data.parent_path());
            app.verifyActivation = true;
            app.page = Page::Journal;
        }
    }
    EndCard();

    Spacer(metric::Gap);
    if (BeginCard("verifications")) {
        Heading("Vérifications");
        Spacer(4);
        CheckRow(state.DotaRunning() ? Mark::Ok : Mark::Warn, "Dota 2 en cours d'exécution",
                 state.DotaRunning() ? "oui" : "non");
        CheckRow(state.DotaInstalled() ? Mark::Ok : Mark::Bad, "Installation de Dota 2 trouvée",
                 state.DotaInstalled() ? "oui" : "introuvable");
        CheckRow(state.BinariesPresent() ? Mark::Ok : Mark::Bad, "Chargeur et bibliothèque",
                 state.BinariesPresent() ? "présents" : "manquants");
        static char catalogue[64];
        if (state.catalog.present)
            sprintf_s(catalogue, "%zu objets · %s", state.catalog.entries, state.catalog.updated.c_str());
        else
            strcpy_s(catalogue, "absent");
        CheckRow(state.catalog.present ? Mark::Ok : Mark::Bad, "Catalogue des objets", catalogue);
        // Python is not needed to use Wardrobe, only for the rare maintenance
        // jobs, so its absence is stated plainly rather than flagged as a fault.
        CheckRow(state.pythonPresent ? Mark::Ok : Mark::Idle, "Python (facultatif, pour l'entretien)",
                 state.pythonPresent ? "installé" : "absent, non requis");
    }
    EndCard();

    Spacer(metric::Gap);
    if (BeginCard("marche")) {
        Heading("Comment ça marche");
        Spacer(6);
        Step(1, "Ouvre Dota 2 et attends le menu principal.");
        Step(2, "Reviens ici et clique sur « Activer Wardrobe ».");
        Step(3, "Équipe tes objets dans le menu de Dota, puis lance une partie.");
    }
    EndCard();

    // Only while the library is loaded is its log a live account of the game;
    // an older line would describe a session that has nothing to do with now.
    if (state.wardrobeActive) {
        if (const char* summary = PhaseSummary(state.appearanceLine)) {
            Spacer(metric::Gap);
            if (BeginCard("rapport")) {
                Heading("Dernier rapport du jeu");
                Spacer(4);
                SmallWrapped(color::Muted, summary);
            }
            EndCard();
        }
    }
}

// Python buffers its output whenever stdout is a pipe, which would leave the
// journal empty for the whole of a long job. -u is honoured by every launcher.
std::wstring Python(const fs::path& interpreter, const fs::path& script, const std::wstring& arguments = {}) {
    return task::Quote(interpreter) + L" -u -X utf8=1 " + task::Quote(script) + (arguments.empty() ? L"" : L" " + arguments);
}

// ---- Entretien ------------------------------------------------------------
// The journal offers the follow-up to this one job by name; keep them together.
constexpr const char* RegenerateProfile = "Régénérer le profil de Dota";

struct Job {
    const char* title;
    const char* description;
    const char* button;
    const char* unavailable;      // why it cannot run now, or nullptr
    std::wstring commandLine;
    fs::path workingDirectory;
    bool destructive = false;
};

void DrawJob(App& app, const Job& job) {
    using namespace ui;
    if (BeginCard(job.title)) {
        const bool available = !job.unavailable && !app.runner.Busy();
        constexpr float buttonWidth = 170.0f;
        const float top = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(top + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::PushFont(fonts().strong, metric::TextBody);
        TextIn(color::Text, job.title);
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - buttonWidth, top));
        if (Ghost(job.button, available, buttonWidth) && available) {
            app.runner.Start(job.title, job.commandLine, job.workingDirectory);
            app.verifyActivation = false;
            app.page = Page::Journal;
        }
        Spacer(2);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - buttonWidth - metric::Pad);
        SmallWrapped(color::Muted, job.description);
        if (job.unavailable) Small(color::Warn, job.unavailable);
        ImGui::PopTextWrapPos();
    }
    EndCard();
}

void DrawMaintenance(App& app, const env::Snapshot& state) {
    using namespace ui;
    Heading("Entretien");
    Spacer(2);
    SmallWrapped(color::Muted,
                 "Ces opérations ne sont utiles qu'après une mise à jour de Dota 2, ou si l'écran d'accueil signale un problème. "
                 "Rien ici n'est nécessaire au quotidien.");
    Spacer(metric::Gap);

    const auto python = state.python;
    const char* noPython = state.pythonPresent ? nullptr : "Python n'est pas installé sur cet ordinateur.";
    const auto refresh = state.layout.Script(L"refresh_profile.py");
    const auto update = state.layout.Script(L"update_db.py");
    const auto repair = state.layout.Script(L"repair_inventory_cache.py");

    DrawJob(app, {"Mettre à jour le catalogue",
                  "À lancer après une mise à jour de contenu de Dota 2. Relit les objets directement dans les fichiers du jeu et "
                  "reconstruit le catalogue. Environ une minute.",
                  "Mettre à jour", update.empty() ? "Le script est introuvable." : noPython,
                  Python(python, update), state.layout.project});

    DrawJob(app, {"Vérifier le profil de Dota",
                  "Vérifie que Wardrobe sait encore retrouver ce dont il a besoin dans la version installée de Dota 2. "
                  "N'écrit rien. Quelques secondes.",
                  "Vérifier", refresh.empty() ? "Le script est introuvable." : noPython,
                  Python(python, refresh, L"--check"), state.layout.project});

    DrawJob(app, {RegenerateProfile,
                  "À ne lancer que si la vérification ci-dessus signale un problème, ou si le jeu affiche « client.dll non reconnu ». "
                  "Le résultat ne prend effet qu'après une recompilation. Environ une minute.",
                  "Régénérer",
                  !state.projectSources
                      ? "Indisponible ici : cette copie ne contient pas le code source, et le profil régénéré ne pourrait pas être appliqué. "
                        "Redemande une version à jour à la personne qui t'a donné Wardrobe."
                      : refresh.empty() ? "Le script est introuvable." : noPython,
                  Python(python, refresh), state.layout.project});

    // Only shown where there is something to compile; a shared copy has binaries only.
    if (state.projectSources)
        DrawJob(app, {"Recompiler Wardrobe",
                      "Reconstruit l'application, le chargeur et la bibliothèque à partir du code source. Nécessite les outils "
                      "de compilation Visual Studio, et que cette fenêtre soit fermée.",
                      "Recompiler",
                      state.compilerPresent ? nullptr : "Les outils de compilation Visual Studio ne sont pas installés.",
                      L"cmd.exe /c " + task::Quote(state.layout.project / "build.bat") + L" --no-pause", state.layout.project});

    Spacer(metric::Gap);
    if (BeginCard("restauration")) {
        const bool possible = state.pythonPresent && !repair.empty() && state.DotaInstalled() && !state.DotaRunning() && !app.runner.Busy();
        constexpr float buttonWidth = 170.0f;
        const float top = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(top + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::PushFont(fonts().strong, metric::TextBody);
        TextIn(color::Text, "Restaurer l'inventaire Steam");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - buttonWidth, top));
        if (Ghost("Restaurer…", possible, buttonWidth) && possible) app.askRestore = true;
        Spacer(2);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - buttonWidth - metric::Pad);
        SmallWrapped(color::Muted,
                     "Dota 2 peut garder en mémoire des objets d'aperçu et continuer à les afficher même sans Wardrobe. "
                     "Cette opération met de côté les fichiers concernés pour que Steam les retélécharge.");
        if (!state.DotaInstalled()) Small(color::Warn, "Installation de Dota 2 introuvable.");
        else if (state.DotaRunning()) Small(color::Warn, "Ferme complètement Dota 2 avant de continuer.");
        else if (!state.pythonPresent) Small(color::Warn, "Python n'est pas installé sur cet ordinateur.");
        ImGui::PopTextWrapPos();
    }
    EndCard();
}

// ---- Journal --------------------------------------------------------------
void DrawJournal(App& app, const env::Snapshot& world) {
    using namespace ui;
    Heading("Journal");
    const auto state = app.runner.CurrentState();
    const std::string title = app.runner.Title();
    if (state == task::State::Idle) {
        SmallWrapped(color::Muted, "Aucune opération lancée depuis l'ouverture de la fenêtre. "
                                   "Ce que fait Wardrobe apparaîtra ici, en clair.");
    } else {
        // An exit code says the tool ran, not that the world changed. Where the
        // change is observable from here, the observation is what gets reported.
        const bool measured = app.verifyActivation && state == task::State::Succeeded;
        const bool active = world.wardrobeActive;
        Mark mark = state == task::State::Running ? Mark::Busy
                    : state == task::State::Failed ? Mark::Bad
                    : measured && !active        ? Mark::Warn
                                                 : Mark::Ok;
        const char* label = state == task::State::Running ? "En cours"
                            : state == task::State::Failed ? "Échec"
                            : measured && active   ? "Vérifié"
                            : measured && !active  ? "Sans effet"
                                                   : "Terminé";
        Pill(mark, label);
        ImGui::SameLine();
        char elapsed[96];
        sprintf_s(elapsed, "%s · %.0f s", title.c_str(), app.runner.Seconds());
        Small(color::Muted, elapsed);
        Spacer(4);
        if (state == task::State::Failed)
            SmallWrapped(color::Bad, "L'opération s'est arrêtée sur une erreur. Le détail est ci-dessous ; "
                                     "rien n'a été modifié au-delà de ce qui y est écrit.");
        else if (measured && active)
            SmallWrapped(color::Ok, "Wardrobe est bien chargé dans Dota 2 : cette fenêtre l'a constaté elle-même. "
                                    "Équipe maintenant tes objets dans le menu de Dota.");
        else if (measured && !active)
            SmallWrapped(color::Warn, "Le chargeur s'est terminé sans erreur, mais Wardrobe n'apparaît pas dans Dota 2. "
                                      "Ferme le jeu, rouvre-le, et réessaie.");
        else if (state == task::State::Succeeded)
            SmallWrapped(color::Muted, "L'outil s'est terminé sans erreur. Ce qu'il a fait est écrit ci-dessous.");
        else if (app.runner.Output().empty())
            SmallWrapped(color::Muted, "L'outil vient de démarrer. Certaines opérations restent silencieuses une "
                                       "quinzaine de secondes avant d'écrire leur première ligne.");
    }
    Spacer(metric::Gap);

    const auto output = app.runner.Output();
    // An empty console is a void; until something has run, say what this is for.
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if (output.empty() && !app.runner.Busy()) {
        if (BeginCard("vide")) {
            SmallWrapped(color::Muted,
                         "Chaque opération lancée depuis Entretien, et chaque activation de Wardrobe, écrit ici ce qu'elle a "
                         "réellement fait. En cas de problème, c'est le texte à lire, ou à transmettre.");
            Spacer(metric::Gap);
            SmallWrapped(color::Faint,
                         "Le jeu tient son propre journal séparément, dans C:\\Temp\\opencode.");
        }
        EndCard();
        Spacer(metric::Gap);
        if (Ghost("Ouvrir le dossier des journaux", true, 260)) OpenFolder("C:\\Temp\\opencode");
        return;
    }
    // Regenerating stops rather than record a value that moved, because only a
    // person can tell a real Dota change from a mis-picked anchor. That person is
    // reading the lines just below, so the answer belongs here and not on a flag
    // that only a console can pass.
    const bool awaitingAccept =
        state == task::State::Failed && title == RegenerateProfile &&
        std::any_of(output.begin(), output.end(),
                    [](const std::string& line) { return line.find("--accept-changes") != std::string::npos; });
    if (awaitingAccept) {
        const auto script = world.layout.Script(L"refresh_profile.py");
        const bool possible = world.pythonPresent && world.projectSources && !script.empty() && !app.runner.Busy();
        if (BeginCard("accepter")) {
            constexpr float buttonWidth = 240.0f;
            const float top = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(top + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::PushFont(fonts().strong, metric::TextBody);
            TextIn(color::Text, "Enregistrer ces valeurs");
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - buttonWidth, top));
            if (Ghost("Enregistrer quand même", possible, buttonWidth) && possible) {
                app.runner.Start(RegenerateProfile, Python(world.python, script, L"--accept-changes"), world.layout.project);
                app.verifyActivation = false;
            }
            Spacer(2);
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - buttonWidth - metric::Pad);
            SmallWrapped(color::Muted,
                         "Des valeurs que Wardrobe avait notées ne sont plus les mêmes dans cette version de Dota 2. "
                         "Le détail est écrit ci-dessous. Si ces lignes ne parlent que de « global », c'est ce que fait "
                         "chaque mise à jour du jeu et il n'y a rien d'inquiétant.");
            if (!possible) Small(color::Warn, "Impossible ici : il manque Python ou le code source du projet.");
            ImGui::PopTextWrapPos();
        }
        EndCard();
        Spacer(metric::Gap);
    }
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, color::Vec(color::Surface));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, metric::CardRadius);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(metric::Pad, metric::Pad));
        ImGui::BeginChild("sortie", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), ImGuiChildFlags_Borders);
        ImGui::PushFont(fonts().mono, metric::TextSmall);
        for (const auto& line : output) {
            ImU32 tint = color::Muted;
            if (line.find("[!]") != std::string::npos || line.find("error") != std::string::npos ||
                line.find("FAIL") != std::string::npos) tint = color::Bad;
            else if (line.find("[OK]") != std::string::npos || line.find("PASS") != std::string::npos) tint = color::Ok;
            else if (line.rfind("[*]", 0) == 0) tint = color::Text;
            TextIn(tint, line.c_str());
        }
        if (app.runner.Busy()) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - footer);
    if (Ghost("Ouvrir le dossier des journaux", true, 260)) OpenFolder("C:\\Temp\\opencode");
    if (app.runner.CurrentState() == task::State::Succeeded || app.runner.CurrentState() == task::State::Failed) {
        ImGui::SameLine();
        if (Ghost("Effacer", true, 120)) app.runner.Acknowledge();
    }
}

// ---- chrome ---------------------------------------------------------------
void DrawRail(App& app, const env::Snapshot& state) {
    using namespace ui;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color::Vec(color::Surface));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, metric::Section));
    ImGui::BeginChild("rail", ImVec2(236, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(ImGui::GetWindowPos().x + 235, ImGui::GetWindowPos().y),
                                              ImVec2(ImGui::GetWindowPos().x + 236, ImGui::GetWindowPos().y + ImGui::GetWindowHeight()),
                                              color::Border);
    ImGui::Indent(12);
    Sized(fonts().strong, metric::TextHeading, color::Text, "Wardrobe");
    Small(color::Faint, "Dota 2");
    ImGui::Unindent(12);
    Spacer(metric::Section);

    const struct { Page page; const char* label; } entries[] = {
        {Page::Home, "Accueil"}, {Page::Maintenance, "Entretien"}, {Page::Journal, "Journal"}};
    for (const auto& entry : entries) {
        const bool active = app.page == entry.page;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton(entry.label, ImVec2(width, 38));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemDeactivated() && hovered) app.page = entry.page;
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (active || hovered)
            draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + 38),
                                active ? color::Raised : color::Fade(color::Raised, 0.5f), metric::ControlRadius);
        if (active) draw->AddRectFilled(origin, ImVec2(origin.x + 3, origin.y + 38), color::Accent, 2.0f);
        draw->AddText(fonts().strong, metric::TextBody, ImVec2(origin.x + 12, origin.y + 10),
                      active ? color::Text : color::Muted, entry.label);
        // A job that ended while the user was elsewhere should say so on the tab.
        if (entry.page == Page::Journal && app.runner.CurrentState() != task::State::Idle) {
            const auto mark = app.runner.Busy() ? Mark::Busy
                              : app.runner.CurrentState() == task::State::Succeeded ? Mark::Ok : Mark::Bad;
            draw->AddCircleFilled(ImVec2(origin.x + width - 16, origin.y + 19), 4.0f, MarkColor(mark), 16);
        }
    }

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 108);
    ImGui::PushStyleColor(ImGuiCol_Separator, color::Vec(color::Border));
    ImGui::Separator();
    ImGui::PopStyleColor();
    Spacer(4);
    ImGui::Indent(12);
    Small(color::Faint, state.wardrobeActive ? "Actif dans Dota 2"
                        : state.DotaRunning() ? "Dota 2 ouvert" : "Dota 2 fermé");
    Spacer(2);
    if (Ghost("Revérifier", true, 120)) app.watcher.Nudge();
    ImGui::Unindent(12);

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void DrawConfirm(App& app, const env::Snapshot& state) {
    using namespace ui;
    if (app.askRestore) { ImGui::OpenPopup("Restaurer l'inventaire Steam"); app.askRestore = false; }
    ImGui::SetNextWindowSize(ImVec2(520, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(metric::Section, metric::Section));
    if (ImGui::BeginPopupModal("Restaurer l'inventaire Steam", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        SmallWrapped(color::Text,
                     "Les fichiers de cache de Dota 2 qui contiennent des objets d'aperçu vont être déplacés dans une sauvegarde, "
                     "puis retirés du jeu. Steam les reconstruira à la prochaine connexion.");
        Spacer(metric::Gap);
        SmallWrapped(color::Muted,
                     "Une copie vérifiée est conservée dans ton dossier utilisateur ; rien n'est supprimé définitivement. "
                     "Les fichiers sans objet d'aperçu ne sont pas touchés.");
        Spacer(metric::Pad);
        if (Ghost("Annuler", true, 150)) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        const float width = ImGui::GetContentRegionAvail().x;
        if (Action("Restaurer", nullptr, true, width)) {
            app.runner.Start("Restauration de l'inventaire Steam",
                             Python(state.python, state.layout.Script(L"repair_inventory_cache.py"),
                                    task::Quote(state.dotaGame) + L" --repair"),
                             state.layout.project);
            app.page = Page::Journal;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void DrawFrame(App& app) {
    const auto state = app.watcher.Current();
    static task::State previous = task::State::Idle;
    const auto now = app.runner.CurrentState();
    if (previous == task::State::Running && now != task::State::Running) app.watcher.Nudge();
    previous = now;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##wardrobe", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    DrawRail(app, state);
    ImGui::SameLine(0, 0);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, ui::metric::Section));
    ImGui::BeginChild("contenu", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    switch (app.page) {
    case Page::Home: DrawHome(app, state); break;
    case Page::Maintenance: DrawMaintenance(app, state); break;
    case Page::Journal: DrawJournal(app, state); break;
    }
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

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring capture;
    int capturePage = 0;
    if (int count = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) {
            if (!wcscmp(argv[i], L"--capture") && i + 1 < count) capture = argv[++i];
            else if (!wcscmp(argv[i], L"--page") && i + 1 < count) capturePage = _wtoi(argv[++i]);
        }
        LocalFree(argv);
    }

    WNDCLASSEXW window{sizeof(window), CS_CLASSDC, Proc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr,
                       L"WardrobeApp", nullptr};
    RegisterClassExW(&window);
    HWND handle = CreateWindowW(window.lpszClassName, L"Wardrobe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                1100, 790, nullptr, nullptr, instance, nullptr);
    if (!CreateDevice(handle)) {
        ReleaseDevice();
        UnregisterClassW(window.lpszClassName, instance);
        MessageBoxW(nullptr, L"Impossible d'initialiser l'affichage (DirectX 11).", L"Wardrobe", MB_ICONERROR);
        return 1;
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(handle, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
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

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawFrame(app);
        ImGui::Render();
        const float clear[4] = {0.075f, 0.082f, 0.098f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (!capture.empty() && ++frames > 12) {
            char path[MAX_PATH]{};
            WideCharToMultiByte(CP_UTF8, 0, capture.c_str(), -1, path, MAX_PATH, nullptr, nullptr);
            SaveFrame(path);
            running = false;
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
