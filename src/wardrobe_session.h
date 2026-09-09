#pragma once
#include <Windows.h>
#include <string>

// A process-lifetime marker, not an autostart entry. Its handle disappears when
// Dota exits; another DLL copy in the same process must not install more hooks.
inline std::string WardrobeSessionName(DWORD pid) {
    return "Local\\WardrobeInstance_" + std::to_string(pid);
}
inline HANDLE ClaimWardrobeSession(DWORD pid) {
    HANDLE marker = CreateMutexA(nullptr, FALSE, WardrobeSessionName(pid).c_str());
    if (marker && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(marker); return nullptr; }
    return marker;
}
inline bool HasWardrobeSession(DWORD pid) {
    HANDLE marker = OpenMutexA(SYNCHRONIZE, FALSE, WardrobeSessionName(pid).c_str());
    if (!marker) return GetLastError() == ERROR_ACCESS_DENIED;
    CloseHandle(marker); return true;
}
