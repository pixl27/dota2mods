// src/loader.cpp — maps wardrobe_dll.dll into dota2.exe WITHOUT LoadLibrary.
// Build: build.bat does it -> build\map.exe
#include <Windows.h>
#include <TlHelp32.h>
#include <iostream>
#include <fstream>
#include <vector>

static DWORD FindPid(const char* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe{ sizeof(pe) };
    DWORD out = 0;
    if (Process32First(snap, &pe)) do {
        if (!_stricmp(pe.szExeFile, exe)) { out = pe.th32ProcessId; break; }
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    return out;
}

// minimal manual map: headers + sections, relocations, imports, entry call
static bool ManualMap(HANDLE proc, const std::vector<uint8_t>& img, void** outEntry) {
    auto dos = (IMAGE_DOS_HEADER*)img.data();
    auto nt = (IMAGE_NT_HEADERS64*)(img.data() + dos->e_lfanew);
    SIZE_T size = nt->OptionalHeader.SizeOfImage;
    uint8_t* remote = (uint8_t*)VirtualAllocEx(proc, nullptr, size,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;

    // headers
    WriteProcessMemory(proc, remote, img.data(), nt->OptionalHeader.SizeOfHeaders, nullptr);
    // sections
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (sec->SizeOfRawData)
            WriteProcessMemory(proc, remote + sec->VirtualAddress,
                img.data() + sec->PointerToRawData, sec->SizeOfRawData, nullptr);
    }
    // relocations
    uintptr_t delta = (uintptr_t)remote - nt->OptionalHeader.ImageBase;
    if (delta && nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
        auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        uint8_t* base = remote + dir.VirtualAddress;
        SIZE_T done = 0;
        while (done < dir.Size) {
            auto blk = (IMAGE_BASE_RELOCATION*)(base + done);
            if (!blk->SizeOfBlock) break;
            int n = (blk->SizeOfBlock - sizeof(*blk)) / sizeof(WORD);
            WORD* rel = (WORD*)(blk + 1);
            for (int i = 0; i < n; i++) {
                if ((rel[i] >> 12) == IMAGE_REL_BASED_DIR64) {
                    uintptr_t at = (uintptr_t)remote + blk->VirtualAddress + (rel[i] & 0xFFF);
                    uintptr_t v = 0;
                    ReadProcessMemory(proc, (void*)at, &v, 8, nullptr);
                    v += delta;
                    WriteProcessMemory(proc, (void*)at, &v, 8, nullptr);
                }
            }
            done += blk->SizeOfBlock;
        }
    }
    // imports — resolve via local LoadLibrary/GetProcAddress then write remote IAT
    // (loader-side resolve is fine: same OS, same DLL versions for system libs)
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
        auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(img.data() + dir.VirtualAddress);
        for (; imp->Name; imp++) {
            const char* dll = (const char*)(img.data() + imp->Name);
            HMODULE local = LoadLibraryA(dll);
            if (!local) return false;
            auto oft = (IMAGE_THUNK_DATA64*)(img.data() + imp->OriginalFirstThunk);
            auto ft = (IMAGE_THUNK_DATA64*)(img.data() + imp->FirstThunk);
            for (; oft->u1.AddressOfData; oft++, ft++) {
                FARPROC fn = nullptr;
                if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG64)
                    fn = GetProcAddress(local, (LPCSTR)(oft->u1.Ordinal & 0xFFFF));
                else {
                    auto by = (IMAGE_IMPORT_BY_NAME*)(img.data() + oft->u1.AddressOfData);
                    fn = GetProcAddress(local, by->Name);
                }
                if (!fn) return false;
                uintptr_t iatAt = (uintptr_t)remote + imp->FirstThunk
                    + ((uint8_t*)ft - (img.data() + imp->FirstThunk));
                WriteProcessMemory(proc, (void*)iatAt, &fn, 8, nullptr);
            }
        }
    }
    // entry
    *outEntry = remote + nt->OptionalHeader.AddressOfEntryPoint;
    // flip image to executable: whole RX is simplest + quiet enough post-header-erase
    DWORD old;
    VirtualProtectEx(proc, remote, size, PAGE_EXECUTE_READ, &old);
    return true;
}

int main(int argc, char** argv) {
    const char* dll = (argc > 1) ? argv[1] : "wardrobe_dll.dll";
    std::ifstream f(dll, std::ios::binary);
    if (!f) { std::cout << "[!] can't open " << dll << "\n"; return 1; }
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), {});

    DWORD pid = FindPid("dota2.exe");
    if (!pid) { std::cout << "[!] start Dota 2 first, baby — then run me.\n"; return 1; }

    HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION
        | PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD, FALSE, pid);
    if (!proc) { std::cout << "[!] OpenProcess failed: " << GetLastError() << "\n"; return 1; }

    void* entry = nullptr;
    if (!ManualMap(proc, img, &entry)) { std::cout << "[!] map failed\n"; return 1; }

    HANDLE th = CreateRemoteThread(proc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)entry, nullptr, 0, nullptr);
    if (!th) { std::cout << "[!] remote thread failed: " << GetLastError() << "\n"; return 1; }
    WaitForSingleObject(th, 5000);
    CloseHandle(th);
    CloseHandle(proc);
    std::cout << "[OK] mapped + running. INSERT in-game for the live menu.\n";
    return 0;
}
