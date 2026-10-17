#pragma once
#include "release.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../version.h"

#pragma comment(lib, "winhttp.lib")

// Updates for copies that were installed, not built: the latest release of the
// GitHub repository is compared with this build's version, and a newer
// installer is downloaded and run in --update mode (it installs without a
// click and reopens Wardrobe). The installer verifies its own payload, so a
// broken download cannot install anything.
namespace wardrobe::update {

namespace fs = std::filesystem;

enum class State { Idle, Checking, Current, Available, Downloading, Ready, Failed };

struct Status {
    State state = State::Idle;
    std::string version;   // of the newer release
    std::string url;       // its installer
    std::string error;
    float progress = 0;
    fs::path installer;    // downloaded file, once Ready
};

inline std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
    std::wstring wide(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), wide.data(), size);
    return wide;
}

// One GET over WinHTTP (HTTPS, redirects followed), body handed to a sink.
// Plain http is accepted only for a test endpoint on 127.0.0.1.
template <typename Sink>
bool Get(const std::string& url, Sink sink, std::string& error, uint64_t* length = nullptr) {
    const std::wstring wide = Wide(url);
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256]{}, path[2048]{};
    parts.lpszHostName = host; parts.dwHostNameLength = DWORD(std::size(host));
    parts.lpszUrlPath = path; parts.dwUrlPathLength = DWORD(std::size(path));
    wchar_t extra[2048]{};
    parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = DWORD(std::size(extra));
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) { error = "adresse invalide"; return false; }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && wcscmp(host, L"127.0.0.1") != 0) { error = "connexion non chiffrée refusée"; return false; }
    HINTERNET session = WinHttpOpen(L"Wardrobe/" WARDROBE_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = "réseau indisponible"; return false; }
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    bool ok = false;
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = nullptr;
    if (connection) {
        const std::wstring target = std::wstring(path) + extra;
        request = WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     secure ? WINHTTP_FLAG_SECURE : 0);
    }
    if (request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json, application/octet-stream\r\n", DWORD(-1),
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (length) {
            wchar_t text[32]{};
            DWORD bytes = sizeof(text);
            *length = WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, text, &bytes,
                                          WINHTTP_NO_HEADER_INDEX) ? _wcstoui64(text, nullptr, 10) : 0;
        }
        if (code == 200) {
            ok = true;
            std::vector<char> buffer(64 * 1024);
            for (;;) {
                DWORD read = 0;
                if (!WinHttpReadData(request, buffer.data(), DWORD(buffer.size()), &read)) { ok = false; error = "téléchargement interrompu"; break; }
                if (!read) break;
                if (!sink(buffer.data(), size_t(read))) { ok = false; error = "écriture impossible"; break; }
            }
        } else {
            error = code == 404 ? "aucune version publiée" : "réponse inattendue du serveur (" + std::to_string(code) + ")";
        }
    } else if (error.empty()) {
        error = "serveur injoignable";
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

inline std::string Endpoint() {
    char forced[512]{};   // tests point this at a local fake release
    if (GetEnvironmentVariableA("WARDROBE_TEST_RELEASES", forced, sizeof(forced))) return forced;
    return "https://api.github.com/repos/" WARDROBE_REPOSITORY "/releases/latest";
}

class Updater {
public:
    explicit Updater(bool enabled) : enabled_(enabled) {
        if (enabled_) worker_ = std::thread([this] { Loop(); });
    }
    ~Updater() {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
        if (download_.joinable()) download_.join();
    }
    Status Current() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }
    void Download() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (status_.state != State::Available && status_.state != State::Failed) return;
            if (status_.url.empty()) return;
            status_.state = State::Downloading;
            status_.progress = 0;
            status_.error.clear();
        }
        if (download_.joinable()) download_.join();
        download_ = std::thread([this] { Fetch(); });
    }

private:
    void Set(const Status& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = value;
    }
    void Check() {
        Status next = Current();
        if (next.state == State::Downloading || next.state == State::Ready) return;
        next.state = State::Checking;
        Set(next);
        std::string body, error;
        const bool ok = Get(Endpoint(), [&](const char* data, size_t size) { body.append(data, size); return body.size() < (8u << 20); }, error);
        Status result;
        if (!ok) { result.state = State::Failed; result.error = error; Set(result); return; }
        const auto release = ParseRelease(body, WARDROBE_VERSION);
        result.version = release.version;
        result.url = release.url;
        switch (release.state) {
        case ReleaseState::Current: result.state = State::Current; break;
        case ReleaseState::Available: result.state = State::Available; break;
        case ReleaseState::MissingInstaller:
            result.state = State::Failed;
            result.error = "la nouvelle version ne contient pas d'installateur Wardrobe compatible";
            break;
        default:
            result.state = State::Failed;
            result.error = "réponse de mise à jour invalide";
            break;
        }
        Set(result);
    }
    void Fetch() {
        Status status = Current();
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        const fs::path target = fs::path(temp) / (L"Wardrobe-Setup-" + Wide(status.version) + L".exe");
        const fs::path partial = fs::path(target).concat(L".part");
        std::string error;
        uint64_t length = 0, received = 0;
        bool ok = false;
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            ok = out && Get(status.url, [&](const char* data, size_t size) {
                out.write(data, std::streamsize(size));
                received += size;
                if (length) { std::lock_guard<std::mutex> lock(mutex_); status_.progress = float(double(received) / double(length)); }
                return bool(out);
            }, error, &length);
        }
        std::error_code ignored;
        if (ok && received > 1024 && MoveFileExW(partial.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.state = State::Ready;
            status_.progress = 1;
            status_.installer = target;
        } else {
            fs::remove(partial, ignored);
            std::lock_guard<std::mutex> lock(mutex_);
            status_.state = State::Failed;
            status_.error = error.empty() ? "téléchargement incomplet" : error;
        }
    }
    void Loop() {
        // At start, then every six hours: the API allows 60 unauthenticated calls an hour.
        while (!stop_) {
            Check();
            for (int i = 0; i < 6 * 3600 * 20 && !stop_; ++i) Sleep(50);
        }
    }
    bool enabled_;
    mutable std::mutex mutex_;
    Status status_;
    std::atomic<bool> stop_{false};
    std::thread worker_, download_;
};

}  // namespace wardrobe::update
