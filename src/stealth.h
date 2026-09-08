// src/stealth.h — Layer 2. Compiled in, OFF by default.
#pragma once
#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <cstring>
#include <string>
#include <map>
#include <cstdio>
#include "db.h"
#pragma comment(lib, "psapi.lib")

namespace stealth {
    struct Offsets {
        uintptr_t dwLocalHero = 0;
        uintptr_t m_hWearables = 0;
        uintptr_t m_ItemView = 0;
        uintptr_t m_iDefIndex = 0;
        uintptr_t m_nFallbackPaint = 0;
        uintptr_t m_bNeedReapply = 0;
        uintptr_t dwEntityList = 0;
        uintptr_t fnFullUpdate = 0;
        bool valid = false;
    };
    inline Offsets g_Off;

    // Direct NT system call typedefs to bypass user-mode API hooks on kernel32
    typedef struct _UNICODE_STRING {
        USHORT Length;
        USHORT MaximumLength;
        PWSTR  Buffer;
    } UNICODE_STRING, *PUNICODE_STRING;

    typedef struct _OBJECT_ATTRIBUTES {
        ULONG Length;
        HANDLE RootDirectory;
        PUNICODE_STRING ObjectName;
        ULONG Attributes;
        PVOID SecurityDescriptor;
        PVOID SecurityQualityOfService;
    } OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

    typedef struct _CLIENT_ID {
        HANDLE UniqueProcess;
        HANDLE UniqueThread;
    } CLIENT_ID, *PCLIENT_ID;

    typedef LONG(NTAPI* pfnNtOpenProcess)(
        PHANDLE ProcessHandle,
        ACCESS_MASK DesiredAccess,
        POBJECT_ATTRIBUTES ObjectAttributes,
        PCLIENT_ID ClientId
    );

    typedef LONG(NTAPI* pfnNtReadVirtualMemory)(
        HANDLE ProcessHandle,
        PVOID BaseAddress,
        PVOID Buffer,
        SIZE_T NumberOfBytesToRead,
        PSIZE_T NumberOfBytesRead
    );

    typedef LONG(NTAPI* pfnNtWriteVirtualMemory)(
        HANDLE ProcessHandle,
        PVOID BaseAddress,
        PVOID Buffer,
        SIZE_T NumberOfBytesToWrite,
        PSIZE_T NumberOfBytesWritten
    );

    typedef LONG(NTAPI* pfnNtClose)(HANDLE Handle);

    struct NtApi {
        pfnNtOpenProcess NtOpenProcess = nullptr;
        pfnNtReadVirtualMemory NtReadVirtualMemory = nullptr;
        pfnNtWriteVirtualMemory NtWriteVirtualMemory = nullptr;
        pfnNtClose NtClose = nullptr;

        NtApi() {
            HMODULE hNt = GetModuleHandleA("ntdll.dll");
            if (hNt) {
                NtOpenProcess = (pfnNtOpenProcess)GetProcAddress(hNt, "NtOpenProcess");
                NtReadVirtualMemory = (pfnNtReadVirtualMemory)GetProcAddress(hNt, "NtReadVirtualMemory");
                NtWriteVirtualMemory = (pfnNtWriteVirtualMemory)GetProcAddress(hNt, "NtWriteVirtualMemory");
                NtClose = (pfnNtClose)GetProcAddress(hNt, "NtClose");
            }
        }
    };

    inline NtApi& GetNt() {
        static NtApi s_Nt;
        return s_Nt;
    }

    inline bool LoadOffsets() {
        const char* paths[] = { "offsets.bin", "build/offsets.bin", "C:\\Temp\\opencode\\offsets.bin" };
        for (auto p : paths) {
            FILE* f = nullptr;
            fopen_s(&f, p, "rb");
            if (!f) continue;
            uint32_t magic = 0, ver = 0;
            uintptr_t vals[8] = {};
            bool ok = fread(&magic, 1, 4, f) == 4 && magic == 0x57415244 &&
                      fread(&ver, 1, 4, f) == 4 &&
                      fread(vals, sizeof(uintptr_t), 8, f) == 8;
            fclose(f);
            if (ok) {
                g_Off.dwLocalHero = vals[0];
                g_Off.m_hWearables = vals[1];
                g_Off.m_ItemView = vals[2];
                g_Off.m_iDefIndex = vals[3];
                g_Off.m_nFallbackPaint = vals[4];
                g_Off.m_bNeedReapply = vals[5];
                g_Off.dwEntityList = vals[6];
                g_Off.fnFullUpdate = vals[7];
                g_Off.valid = true;
                return true;
            }
        }
        return false;
    }

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
        auto& nt = GetNt();
        if (nt.NtOpenProcess) {
            HANDLE h = nullptr;
            OBJECT_ATTRIBUTES oa{};
            oa.Length = sizeof(oa);
            CLIENT_ID cid{};
            cid.UniqueProcess = (HANDLE)(uintptr_t)pid;
            LONG st = nt.NtOpenProcess(&h, PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, &oa, &cid);
            if (st >= 0 && h) return h;
        }
        return OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    }

    inline void CloseProcHandle(HANDLE h) {
        auto& nt = GetNt();
        if (nt.NtClose) nt.NtClose(h);
        else CloseHandle(h);
    }

    template<typename T>
    inline bool ReadMem(HANDLE h, uintptr_t addr, T* outVal) {
        auto& nt = GetNt();
        SIZE_T read = 0;
        if (nt.NtReadVirtualMemory) {
            return nt.NtReadVirtualMemory(h, (PVOID)addr, outVal, sizeof(T), &read) >= 0;
        }
        return ReadProcessMemory(h, (void*)addr, outVal, sizeof(T), &read) != FALSE;
    }

    template<typename T>
    inline bool WriteMem(HANDLE h, uintptr_t addr, const T& inVal) {
        auto& nt = GetNt();
        SIZE_T written = 0;
        if (nt.NtWriteVirtualMemory) {
            return nt.NtWriteVirtualMemory(h, (PVOID)addr, (PVOID)&inVal, sizeof(T), &written) >= 0;
        }
        return WriteProcessMemory(h, (void*)addr, &inVal, sizeof(T), &written) != FALSE;
    }

    inline uintptr_t GetRemoteModuleBase(DWORD pid, const char* modName) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        MODULEENTRY32 me{ sizeof(me) };
        uintptr_t base = 0;
        if (Process32First(snap, &me)) {
            do {
                if (!_stricmp(me.szModule, modName)) {
                    base = (uintptr_t)me.modBaseAddr;
                    break;
                }
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
        return base;
    }

    inline uintptr_t RemoteHandleToEnt(HANDLE h, uintptr_t entList, uint32_t handle) {
        uint32_t idx = handle & 0xFFF;
        uintptr_t chunk = 0;
        if (!ReadMem(h, entList + 0x8 * (idx >> 9) + 0x10, &chunk) || !chunk) return 0;
        uintptr_t ent = 0;
        if (!ReadMem(h, chunk + 0x70 * (idx & 0x1FF), &ent)) return 0;
        return ent;
    }

    inline bool PushLoadout(const std::string& heroKey) {
        if (!g_Off.valid) return false;
        DWORD pid = FindDotaPid();
        if (!pid) return false;
        HANDLE h = SpoofedHandle(pid);
        if (!h || h == INVALID_HANDLE_VALUE) return false;

        uintptr_t clientBase = GetRemoteModuleBase(pid, "client.dll");
        if (!clientBase) {
            CloseProcHandle(h);
            return false;
        }

        uintptr_t hero = 0;
        if (!ReadMem(h, clientBase + g_Off.dwLocalHero, &hero) || !hero) {
            CloseProcHandle(h);
            return false;
        }

        std::map<std::string, int> wants;
        {
            std::lock_guard<std::mutex> lk(g_LoadoutMutex);
            auto it = g_Loadout.find(heroKey);
            if (it == g_Loadout.end()) {
                CloseProcHandle(h);
                return false;
            }
            wants = it->second;
        }

        uintptr_t entList = 0;
        if (!ReadMem(h, clientBase + g_Off.dwEntityList, &entList) || !entList) {
            CloseProcHandle(h);
            return false;
        }

        const char* order[] = { "head","shoulder","arms","belt","weapon","mount","ambient","ward" };
        for (int i = 0; i < 8; i++) {
            auto w = wants.find(order[i]);
            if (w == wants.end() || w->second < 0) continue;

            uint32_t handle = 0;
            if (!ReadMem(h, hero + g_Off.m_hWearables + i * 4, &handle) || handle == 0xFFFFFFFF || handle == 0)
                continue;

            uintptr_t wearable = RemoteHandleToEnt(h, entList, handle);
            if (!wearable) continue;

            uintptr_t itemView = wearable + g_Off.m_ItemView;
            int32_t defIndex = (int32_t)w->second;
            WriteMem(h, itemView + g_Off.m_iDefIndex, defIndex);

            if (g_Off.m_nFallbackPaint) {
                int32_t zero = 0;
                WriteMem(h, itemView + g_Off.m_nFallbackPaint, zero);
            }
        }

        if (g_Off.m_bNeedReapply) {
            bool reapply = true;
            WriteMem(h, hero + g_Off.m_bNeedReapply, reapply);
        }

        CloseProcHandle(h);
        return true;
    }
}
