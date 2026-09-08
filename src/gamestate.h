// src/gamestate.h
#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <atomic>
#include <fstream>
#include <mutex>
#include "db.h"
#pragma comment(lib, "ws2_32.lib")

struct LiveState {
    mutable std::mutex mtx;
    std::string hero;
    std::string gameState;
    int playerSlot = -1;
    std::atomic<bool> inMatch{ false };

    std::string getHero() const {
        std::lock_guard<std::mutex> lk(mtx);
        return hero;
    }
    void setHero(const std::string& h) {
        std::lock_guard<std::mutex> lk(mtx);
        hero = h;
    }
    std::string getGameState() const {
        std::lock_guard<std::mutex> lk(mtx);
        return gameState;
    }
    void setGameState(const std::string& s) {
        std::lock_guard<std::mutex> lk(mtx);
        gameState = s;
    }
    int getPlayerSlot() const {
        std::lock_guard<std::mutex> lk(mtx);
        return playerSlot;
    }
    void setPlayerSlot(int p) {
        std::lock_guard<std::mutex> lk(mtx);
        playerSlot = p;
    }
};
extern LiveState g_Live;

inline void GSIThread() {
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return;

    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) return;

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((u_short)g_Cfg.gsiPort);

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    if (bind(srv, (sockaddr*)&a, sizeof(a)) != 0) {
        closesocket(srv);
        return;
    }
    listen(srv, 8);

    char buf[4096];
    while (true) {
        SOCKET c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;

        DWORD timeout = 2500;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

        std::string req;
        size_t headerEnd = std::string::npos;
        int contentLength = -1;

        while (true) {
            int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, n);

            if (headerEnd == std::string::npos) {
                headerEnd = req.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    std::string headers = req.substr(0, headerEnd);
                    std::string clTag = "Content-Length:";
                    auto pos = headers.find(clTag);
                    if (pos == std::string::npos) {
                        clTag = "content-length:";
                        pos = headers.find(clTag);
                    }
                    if (pos != std::string::npos) {
                        size_t startVal = pos + clTag.length();
                        while (startVal < headers.length() && (headers[startVal] == ' ' || headers[startVal] == '\t'))
                            startVal++;
                        size_t endVal = headers.find("\r\n", startVal);
                        if (endVal != std::string::npos) {
                            try {
                                contentLength = std::stoi(headers.substr(startVal, endVal - startVal));
                            } catch (...) { contentLength = -1; }
                        }
                    }
                }
            }

            if (headerEnd != std::string::npos && contentLength >= 0) {
                size_t bodyReceived = req.size() - (headerEnd + 4);
                if (bodyReceived >= (size_t)contentLength) break;
            }
        }

        if (headerEnd != std::string::npos && req.find(g_Cfg.gsiToken) != std::string::npos) {
            std::string body = req.substr(headerEnd + 4);
            if (contentLength > 0 && body.size() > (size_t)contentLength) {
                body.resize(contentLength);
            }
            try {
                json j = json::parse(body);
                if (j.contains("hero") && j["hero"].contains("name"))
                    g_Live.setHero(j["hero"]["name"].get<std::string>());
                if (j.contains("map") && j["map"].contains("game_state")) {
                    std::string st = j["map"]["game_state"].get<std::string>();
                    g_Live.setGameState(st);
                    g_Live.inMatch =
                        st == "DOTA_GAMERULES_STATE_GAME_IN_PROGRESS" ||
                        st == "DOTA_GAMERULES_STATE_PRE_GAME" ||
                        st == "DOTA_GAMERULES_STATE_HERO_SELECTION";
                }
                if (j.contains("player") && j["player"].contains("player_slot"))
                    g_Live.setPlayerSlot(j["player"]["player_slot"].get<int>());
            } catch (...) {}
        }
        const char* ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        send(c, ok, (int)strlen(ok), 0);
        closesocket(c);
    }
}
