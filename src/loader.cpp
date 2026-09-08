// src/loader.cpp — maps wardrobe_dll.dll into dota2.exe WITHOUT LoadLibrary.
// Build: build.bat does it -> build\map.exe
#include <Windows.h>
#include <TlHelp32.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>

static DWORD FindPid(const char* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{ sizeof(pe) };
    DWORD out = 0;
    if (Process32First(snap, &pe)) do {
        if (!_stricmp(pe.szExeFile, exe)) { out = pe.th32ProcessId; break; }
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);
    return out;
}

// robust manual map: local stage -> relocate -> resolve imports -> write remote -> stub execute
static bool ManualMap(HANDLE proc, const std::vector<uint8_t>& img, uint8_t** outRemoteBase, uintptr_t* outEntry) {
    if (img.size() < sizeof(IMAGE_DOS_HEADER)) return false;

    auto dos = (const IMAGE_DOS_HEADER*)img.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    if (dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > img.size()) return false;
    auto nt = (const IMAGE_NT_HEADERS64*)(img.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        std::cout << "[!] Target image must be 64-bit.\n";
        return false;
    }

    SIZE_T imageSize = nt->OptionalHeader.SizeOfImage;
    uint8_t* remoteBase = (uint8_t*)VirtualAllocEx(proc, nullptr, imageSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteBase) {
        std::cout << "[!] VirtualAllocEx failed: " << GetLastError() << "\n";
        return false;
    }

    // Allocate local buffer to stage the memory layout before writing to remote
    std::vector<uint8_t> local(imageSize, 0);

    // 1. Copy headers
    size_t headerSize = min((size_t)nt->OptionalHeader.SizeOfHeaders, img.size());
    memcpy(local.data(), img.data(), headerSize);

    // 2. Copy sections into virtual layout
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (sec->SizeOfRawData && sec->PointerToRawData < img.size()) {
            size_t copySize = (size_t)sec->SizeOfRawData;
            if (sec->PointerToRawData + copySize > img.size()) {
                copySize = img.size() - sec->PointerToRawData;
            }
            if (sec->VirtualAddress + copySize <= imageSize) {
                memcpy(local.data() + sec->VirtualAddress, img.data() + sec->PointerToRawData, copySize);
            }
        }
    }

    // 3. Base relocations
    uintptr_t delta = (uintptr_t)remoteBase - nt->OptionalHeader.ImageBase;
    if (delta != 0 && nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
        auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (dir.VirtualAddress + dir.Size <= imageSize) {
            uint8_t* relocBase = local.data() + dir.VirtualAddress;
            SIZE_T done = 0;
            while (done < dir.Size) {
                auto blk = (IMAGE_BASE_RELOCATION*)(relocBase + done);
                if (!blk->SizeOfBlock || blk->SizeOfBlock > (dir.Size - done)) break;
                int count = (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                WORD* list = (WORD*)(blk + 1);
                for (int i = 0; i < count; i++) {
                    int type = list[i] >> 12;
                    int offset = list[i] & 0xFFF;
                    if (type == IMAGE_REL_BASED_DIR64) {
                        uintptr_t patchRva = blk->VirtualAddress + offset;
                        if (patchRva + sizeof(uintptr_t) <= imageSize) {
                            uintptr_t* patchAddr = (uintptr_t*)(local.data() + patchRva);
                            *patchAddr += delta;
                        }
                    }
                }
                done += blk->SizeOfBlock;
            }
        }
    }

    // 4. Resolve imports in local image using local system libraries
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
        auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress < imageSize) {
            auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(local.data() + dir.VirtualAddress);
            for (; imp->Name && imp->Name < imageSize; imp++) {
                const char* dllName = (const char*)(local.data() + imp->Name);
                HMODULE hMod = LoadLibraryA(dllName);
                if (!hMod) {
                    std::cout << "[!] Could not load import module: " << dllName << "\n";
                    VirtualFreeEx(proc, remoteBase, 0, MEM_RELEASE);
                    return false;
                }

                uint32_t thunkRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
                if (!thunkRva || thunkRva >= imageSize || imp->FirstThunk >= imageSize) continue;

                auto origThunk = (IMAGE_THUNK_DATA64*)(local.data() + thunkRva);
                auto firstThunk = (IMAGE_THUNK_DATA64*)(local.data() + imp->FirstThunk);

                for (; origThunk->u1.AddressOfData; origThunk++, firstThunk++) {
                    FARPROC fn = nullptr;
                    if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) {
                        fn = GetProcAddress(hMod, (LPCSTR)(origThunk->u1.Ordinal & 0xFFFF));
                    } else {
                        uint32_t nameRva = (uint32_t)origThunk->u1.AddressOfData;
                        if (nameRva < imageSize) {
                            auto byName = (IMAGE_IMPORT_BY_NAME*)(local.data() + nameRva);
                            fn = GetProcAddress(hMod, byName->Name);
                        }
                    }
                    if (!fn) {
                        std::cout << "[!] Failed to resolve symbol in " << dllName << "\n";
                        VirtualFreeEx(proc, remoteBase, 0, MEM_RELEASE);
                        return false;
                    }
                    firstThunk->u1.Function = (ULONGLONG)fn;
                }
            }
        }
    }

    // 5. Write staged image into remote process memory
    if (!WriteProcessMemory(proc, remoteBase, local.data(), imageSize, nullptr)) {
        std::cout << "[!] WriteProcessMemory failed: " << GetLastError() << "\n";
        VirtualFreeEx(proc, remoteBase, 0, MEM_RELEASE);
        return false;
    }

    *outRemoteBase = remoteBase;
    *outEntry = (uintptr_t)remoteBase + nt->OptionalHeader.AddressOfEntryPoint;
    return true;
}

int main(int argc, char** argv) {
    const char* dll = (argc > 1) ? argv[1] : "wardrobe_dll.dll";
    std::ifstream f(dll, std::ios::binary);
    if (!f) { std::cout << "[!] can't open " << dll << "\n"; return 1; }
    std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), {});

    DWORD pid = FindPid("dota2.exe");
    if (!pid) { std::cout << "[!] start Dota 2 first, then run map.exe.\n"; return 1; }

    HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION
        | PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD, FALSE, pid);
    if (!proc) { std::cout << "[!] OpenProcess failed: " << GetLastError() << "\n"; return 1; }

    uint8_t* remoteBase = nullptr;
    uintptr_t entry = 0;
    if (!ManualMap(proc, img, &remoteBase, &entry)) {
        std::cout << "[!] Manual map staging failed.\n";
        CloseHandle(proc);
        return 1;
    }

    // Allocate a small remote stub to invoke DllMain((HINSTANCE)remoteBase, DLL_PROCESS_ATTACH, nullptr)
    // Stub byte assembly for x64:
    //   48 83 EC 28                 sub rsp, 28h
    //   48 B9 <remoteBase: 8 bytes> mov rcx, remoteBase
    //   BA 01 00 00 00              mov edx, 1
    //   45 31 C0                    xor r8d, r8d
    //   48 B8 <entry: 8 bytes>      mov rax, entry
    //   FF D0                       call rax
    //   48 83 C4 28                 add rsp, 28h
    //   C3                          ret
    uint8_t stubCode[] = {
        0x48, 0x83, 0xEC, 0x28,
        0x48, 0xB9, 0,0,0,0,0,0,0,0,
        0xBA, 0x01, 0x00, 0x00, 0x00,
        0x45, 0x31, 0xC0,
        0x48, 0xB8, 0,0,0,0,0,0,0,0,
        0xFF, 0xD0,
        0x48, 0x83, 0xC4, 0x28,
        0xC3
    };
    memcpy(&stubCode[6], &remoteBase, sizeof(remoteBase));
    memcpy(&stubCode[24], &entry, sizeof(entry));

    uint8_t* remoteStub = (uint8_t*)VirtualAllocEx(proc, nullptr, sizeof(stubCode),
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteStub) {
        std::cout << "[!] Remote stub allocation failed: " << GetLastError() << "\n";
        CloseHandle(proc);
        return 1;
    }

    WriteProcessMemory(proc, remoteStub, stubCode, sizeof(stubCode), nullptr);

    HANDLE th = CreateRemoteThread(proc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)remoteStub, nullptr, 0, nullptr);
    if (!th) {
        std::cout << "[!] CreateRemoteThread failed: " << GetLastError() << "\n";
        VirtualFreeEx(proc, remoteStub, 0, MEM_RELEASE);
        CloseHandle(proc);
        return 1;
    }

    WaitForSingleObject(th, 6000);
    CloseHandle(th);

    VirtualFreeEx(proc, remoteStub, 0, MEM_RELEASE);
    CloseHandle(proc);

    std::cout << "[OK] mapped + initialized. Press INSERT in-game to toggle status menu.\n";
    return 0;
}
