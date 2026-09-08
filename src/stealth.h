// src/stealth.h — Layer 2. Compiled in, OFF by default.
#pragma once
#include <Windows.h>
#include <Psapi.h>
#include <cstring>
#include <string>
#pragma comment(lib, "psapi.lib")

namespace stealth {
    struct Offsets {
        uintptr_t dwLocalHero = 0;
        uintptr_t m_hWearables = 0;
        uintptr_t m_ItemView = 0;
        uintptr_t m_iDefIndex = 0;
        uintptr_t m_bNeedReapply = 0;
        bool valid = false;   // stays false until YOUR dumper fills it. No blind writes.
    };
    inline Offsets g_Off;

    inline DWORD FindDotaPid() {
        DWORD pids[1024]{}, need = 0;
        if (!EnumProcesses(pids, sizeof(pids), &need)) return 0;
        for (DWORD i = 0; i < need / sizeof(DWORD); i++) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
            if (!h) continue;
            char name[MAX_PATH]{}; DWORD sz = sizeof(name);
            if (QueryFullProcessImageNameA(h, 0, name, &sz) && strstr(name, "dota2.exe")) {
                CloseHandle(h);
                return pids[i];
            }
            CloseHandle(h);
        }
        return 0;
    }

    inline HANDLE SpoofedHandle(DWORD pid) {
        return OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION
            | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    }

    // Fill g_Off from your dumper, then this pushes g_Loadout[hero] live.
    inline bool PushLoadout(const std::string& hero);
}
