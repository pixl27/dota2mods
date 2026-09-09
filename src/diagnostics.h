#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>

// Diagnostic logs append forever otherwise. Rotate once past the limit so a
// long session (or a chatty failure) cannot fill the disk or hide the recent
// lines behind megabytes of history.
inline void RotateLogIfLarge(const char* path, DWORD limitBytes = 4u << 20) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attributes)) return;
    if (attributes.nFileSizeHigh || attributes.nFileSizeLow < limitBytes) return;
    const std::string previous = std::string(path) + ".old";
    MoveFileExA(path, previous.c_str(), MOVEFILE_REPLACE_EXISTING);
}
