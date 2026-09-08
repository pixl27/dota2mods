// src/gamestate.h
#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <atomic>
#include <fstream>
#include "db.h"
#pragma comment(lib, "ws2_32.lib")

struct LiveState {
    std::string hero;
    std::string gameState;
    int playerSlot = -1;
    std::atomic<bool> inMatch{ false };
};
extern LiveState g_Live;

inline void GSIThread() {
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(g_Cfg.gsiPort);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    bind(srv, (sockaddr*)&a, sizeof(a));
    listen(srv, 4);
    char buf[65536];
    while (true) {
        SOCKET c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        int n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            std::string req(buf, n);
            auto bp = req.find("\r\n\r\n");
            if (bp != std::string::npos && req.find(g_Cfg.gsiToken) != std::string::npos) {
                try {
                    json j = json::parse(req.substr(bp + 4));
                    if (j.contains("hero") && j["hero"].contains("name"))
                        g_Live.hero = j["hero"]["name"].get<std::string>();
                    if (j.contains("map") && j["map"].contains("game_state")) {
                        g_Live.gameState = j["map"]["game_state"].get<std::string>();
                        g_Live.inMatch =
                            g_Live.gameState == "DOTA_GAMERULES_STATE_GAME_IN_PROGRESS" ||
                            g_Live.gameState == "DOTA_GAMERULES_STATE_PRE_GAME" ||
                            g_Live.gameState == "DOTA_GAMERULES_STATE_HERO_SELECTION";
                    }
                    if (j.contains("player") && j["player"].contains("player_slot"))
                        g_Live.playerSlot = j["player"]["player_slot"].get<int>();
                } catch (...) {}
            }
        }
        const char* ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        send(c, ok, (int)strlen(ok), 0);
        closesocket(c);
    }
}
