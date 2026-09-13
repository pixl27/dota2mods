#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// One background job at a time, with its output captured instead of thrown at a
// console window the user never asked for. Nothing here reports success on its
// own: only the child's exit code decides.
namespace wardrobe::task {

namespace fs = std::filesystem;

enum class State { Idle, Running, Succeeded, Failed };

// Console tools here emit UTF-8 (Python) or the OEM code page (cmd), and mixing
// them up turns every accent into noise, so the encoding is detected per chunk.
inline std::string ToUtf8(const char* bytes, size_t size) {
    if (!size) return {};
    auto convert = [&](UINT codepage, DWORD flags) -> std::string {
        const int wide = MultiByteToWideChar(codepage, flags, bytes, int(size), nullptr, 0);
        if (!wide) return {};
        std::wstring text(wide, L'\0');
        MultiByteToWideChar(codepage, flags, bytes, int(size), text.data(), wide);
        const int narrow = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), wide, nullptr, 0, nullptr, nullptr);
        std::string result(narrow, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), wide, result.data(), narrow, nullptr, nullptr);
        return result;
    };
    if (auto utf8 = convert(CP_UTF8, MB_ERR_INVALID_CHARS); !utf8.empty()) return utf8;
    return convert(CP_OEMCP, 0);
}

class Runner {
public:
    static constexpr size_t MaxLines = 2000;

    ~Runner() { Detach(); }

    bool Busy() const { return state_.load() == State::Running; }
    State CurrentState() const { return state_.load(); }
    int ExitCode() const { return exitCode_.load(); }

    std::string Title() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return title_;
    }
    std::vector<std::string> Output() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {lines_.begin(), lines_.end()};
    }
    double Seconds() const {
        const auto end = Busy() ? std::chrono::steady_clock::now() : finished_;
        return std::chrono::duration<double>(end - started_).count();
    }

    // A finished job stays on screen until the user takes it in.
    void Acknowledge() {
        if (Busy()) return;
        Detach();
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::Idle;
        title_.clear();
        lines_.clear();
    }

    bool Start(std::string title, const std::wstring& commandLine, const fs::path& workingDirectory) {
        if (Busy()) return false;
        Detach();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            title_ = std::move(title);
            lines_.clear();
        }
        started_ = std::chrono::steady_clock::now();
        state_ = State::Running;
        exitCode_ = -1;
        worker_ = std::thread(&Runner::Run, this, commandLine, workingDirectory);
        return true;
    }

private:
    void Detach() {
        if (worker_.joinable()) worker_.join();
    }

    void Append(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (char character : text) {
            if (character == '\r') continue;
            if (character == '\n') {
                lines_.push_back(pending_);
                pending_.clear();
                if (lines_.size() > MaxLines) lines_.pop_front();
            } else {
                pending_.push_back(character);
            }
        }
    }

    void Finish(State state, const std::string& note) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending_.empty()) { lines_.push_back(pending_); pending_.clear(); }
            if (!note.empty()) lines_.push_back(note);
        }
        finished_ = std::chrono::steady_clock::now();
        state_ = state;
    }

    void Run(std::wstring commandLine, fs::path workingDirectory) {
        {
            const int size = WideCharToMultiByte(CP_UTF8, 0, commandLine.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string readable;
            if (size > 1) {
                readable.resize(size_t(size));   // room for the terminator the API writes
                WideCharToMultiByte(CP_UTF8, 0, commandLine.c_str(), -1, readable.data(), size, nullptr, nullptr);
                readable.resize(size_t(size) - 1);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.push_back("> " + readable);
        }
        SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 1 << 16)) {
            Finish(State::Failed, "Impossible de creer le canal de sortie.");
            return;
        }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = startup.hStdError = writeEnd;
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                 OPEN_EXISTING, 0, nullptr);
        startup.hStdInput = nul;
        PROCESS_INFORMATION process{};
        std::wstring mutable_ = commandLine;
        // The child inherits our environment untouched: hand-built blocks are easy
        // to get subtly wrong, and everything we need is on the command line.
        const BOOL started = CreateProcessW(nullptr, mutable_.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                            &startup, &process);
        CloseHandle(writeEnd);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!started) {
            CloseHandle(readEnd);
            Finish(State::Failed, "Impossible de lancer l'outil. Verifie qu'il est bien present.");
            return;
        }
        std::string buffer(4096, '\0');
        DWORD read = 0;
        while (ReadFile(readEnd, buffer.data(), DWORD(buffer.size()), &read, nullptr) && read)
            Append(ToUtf8(buffer.data(), read));
        CloseHandle(readEnd);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        exitCode_ = int(code);
        Finish(code == 0 ? State::Succeeded : State::Failed, {});
    }

    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
    std::string pending_, title_;
    std::atomic<State> state_{State::Idle};
    std::atomic<int> exitCode_{-1};
    std::chrono::steady_clock::time_point started_{}, finished_{};
    std::thread worker_;
};

// Quoting matters: every one of these paths can contain a space.
inline std::wstring Quote(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }

}  // namespace wardrobe::task
