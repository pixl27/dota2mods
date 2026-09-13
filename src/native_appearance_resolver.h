#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "native_appearance_profile.h"

// A Dota update moves code far more often than it changes it, so nothing here
// trusts a recorded address. Every function is re-found by the bytes around its
// entry (immediates and branch targets masked out), the few that have no unique
// signature by the call instruction that reaches them, and every global, struct
// offset and vtable slot is re-read from the instruction that carries it. The
// profile's addresses are only hints: they make the common case a comparison
// instead of a search. This reads the image and writes nothing to the game.
namespace appearance::profile {

struct Resolution {
    bool resolved = false;
    bool exact = false;      // byte for byte the build the profile was recorded from
    uint32_t moved = 0;      // entries that were no longer at their recorded address
    uint32_t searched = 0;   // entries that needed a search rather than a hint
    char failure[96]{};
};

namespace detail {

struct Pattern {
    const uint8_t* bytes;
    const uint8_t* mask;
    size_t size;
    size_t anchor;           // index of a byte the mask actually compares
};

// The anchor drives a memchr over the whole section, so prefer a compared byte
// that is not 0x00: padding would otherwise make every search walk the image.
inline Pattern MakePattern(const uint8_t* bytes, const uint8_t* mask, size_t size) {
    size_t anchor = size;
    for (size_t i = 0; i < size; ++i) {
        if (mask[i] != 0xff) continue;
        if (bytes[i] != 0) return {bytes, mask, size, i};
        if (anchor == size) anchor = i;
    }
    return {bytes, mask, size, anchor < size ? anchor : 0};
}

inline bool MatchesAt(const uint8_t* at, const Pattern& pattern) {
    for (size_t i = 0; i < pattern.size; ++i)
        if ((at[i] & pattern.mask[i]) != (pattern.bytes[i] & pattern.mask[i])) return false;
    return true;
}

// Visit every match in [begin, begin + length); stop early when visit returns false.
template <class Visit>
void ForEachMatch(const uint8_t* begin, size_t length, const Pattern& pattern, Visit visit) {
    if (!begin || pattern.size == 0 || pattern.size > length) return;
    const int needle = pattern.bytes[pattern.anchor];
    for (size_t at = 0; at + pattern.size <= length;) {
        const size_t span = length - pattern.size + 1 - at;
        const auto found = static_cast<const uint8_t*>(memchr(begin + at + pattern.anchor, needle, span));
        if (!found) return;
        const size_t start = size_t(found - begin) - pattern.anchor;
        if (MatchesAt(begin + start, pattern) && !visit(start)) return;
        at = start + 1;
    }
}

inline int64_t ReadOperand(const uint8_t* window, const Site& site) {
    if (site.width == 1) return int8_t(window[site.operand]);
    if (site.width == 2) { int16_t value = 0; memcpy(&value, window + site.operand, 2); return value; }
    int32_t value = 0;
    memcpy(&value, window + site.operand, 4);
    return value;
}

// rip- and call-relative operands are measured from the end of their instruction.
inline uintptr_t Decode(const Site& site, const uint8_t* window, uintptr_t windowRva) {
    const int64_t operand = ReadOperand(window, site);
    switch (site.kind) {
    case SiteKind::Rip:
    case SiteKind::Call: return uintptr_t(int64_t(windowRva) + site.end + operand);
    case SiteKind::Slot: return uintptr_t(operand / 8);
    default: return uintptr_t(operand);
    }
}

}  // namespace detail

class Resolver {
public:
    Resolution Run(uintptr_t base) {
        Resolution result;
        if (!Load(base)) { Fail(result, "headers", "client.dll is not a 64-bit image"); return result; }
        result.exact = header_.FileHeader.TimeDateStamp == Timestamp && header_.OptionalHeader.SizeOfImage == ImageSize;
        for (const auto& signature : Functions)
            if (!ResolveFunction(signature, result)) return result;
        for (const auto& site : Sites)
            if (!ResolveSite(site, result)) return result;
        result.resolved = true;
        return result;
    }

private:
    uintptr_t base_ = 0;
    IMAGE_NT_HEADERS64 header_{};
    const IMAGE_SECTION_HEADER* sections_ = nullptr;
    uint16_t sectionCount_ = 0;
    const uint8_t* text_ = nullptr;
    uintptr_t textRva_ = 0;
    size_t textSize_ = 0;
    const RUNTIME_FUNCTION* unwind_ = nullptr;
    size_t unwindCount_ = 0;

    static void Fail(Resolution& result, const char* name, const char* reason) {
        sprintf_s(result.failure, "%.40s: %.45s", name, reason);
    }

    bool Load(uintptr_t base) {
        base_ = base;
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
        // The section table is walked before anything else is trusted, so its
        // shape is checked rather than assumed.
        if (nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64)) return false;
        if (!nt->FileHeader.NumberOfSections || nt->FileHeader.NumberOfSections > 96) return false;
        header_ = *nt;
        sections_ = IMAGE_FIRST_SECTION(nt);
        sectionCount_ = nt->FileHeader.NumberOfSections;
        for (uint16_t i = 0; i < sectionCount_; ++i)
            if (!memcmp(sections_[i].Name, ".text", 6)) {
                textRva_ = sections_[i].VirtualAddress;
                textSize_ = sections_[i].Misc.VirtualSize;
                text_ = reinterpret_cast<const uint8_t*>(base + textRva_);
            }
        const auto& exceptions = header_.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (exceptions.VirtualAddress && exceptions.Size) {
            unwind_ = reinterpret_cast<const RUNTIME_FUNCTION*>(base + exceptions.VirtualAddress);
            unwindCount_ = exceptions.Size / sizeof(RUNTIME_FUNCTION);
        }
        return text_ != nullptr && textSize_ > 0;
    }

    bool InText(uintptr_t rva) const { return rva >= textRva_ && rva < textRva_ + textSize_; }
    bool InImage(uintptr_t rva) const { return rva > 0 && rva < header_.OptionalHeader.SizeOfImage; }
    const uint8_t* At(uintptr_t rva) const { return reinterpret_cast<const uint8_t*>(base_ + rva); }

    // A function reaches up to the next entry that starts one: chained unwind
    // records continue the function they follow rather than beginning another.
    uintptr_t FunctionEnd(uintptr_t rva) const {
        constexpr uintptr_t Fallback = 0x2000, Cap = 0x8000;
        if (!unwind_ || !unwindCount_) return rva + Fallback;
        size_t low = 0, high = unwindCount_;
        while (low < high) {
            const size_t middle = low + (high - low) / 2;
            if (unwind_[middle].BeginAddress <= rva) low = middle + 1; else high = middle;
        }
        for (size_t i = low; i < unwindCount_; ++i) {
            if (!InImage(unwind_[i].UnwindData)) continue;
            const auto info = At(unwind_[i].UnwindData);
            if (unwind_[i].BeginAddress > rva && !((info[0] >> 3) & UNW_FLAG_CHAININFO))
                return unwind_[i].BeginAddress > rva + Cap ? rva + Cap : unwind_[i].BeginAddress;
        }
        return rva + Fallback;
    }

    bool ResolveFunction(const Signature& signature, Resolution& result) {
        const auto pattern = detail::MakePattern(signature.bytes, signature.mask, signature.size);
        if (InText(*signature.target) && detail::MatchesAt(At(*signature.target), pattern)) return true;
        // The hint is stale: the signature has to be unique in .text to be trusted.
        uintptr_t found = 0;
        unsigned hits = 0;
        detail::ForEachMatch(text_, textSize_, pattern, [&](size_t at) {
            found = textRva_ + at;
            return ++hits < 2;
        });
        if (hits != 1) {
            Fail(result, signature.name, hits ? "signature is ambiguous" : "signature not found");
            return false;
        }
        *signature.target = found;
        ++result.moved;
        ++result.searched;
        return true;
    }

    bool Accept(const Site& site, const uint8_t* window, uintptr_t windowRva, uintptr_t& value) const {
        value = detail::Decode(site, window, windowRva);
        switch (site.kind) {
        case SiteKind::Call: if (!InText(value)) return false; break;
        case SiteKind::Rip: if (!InImage(value)) return false; break;
        case SiteKind::Slot: if (value > 0x400) return false; break;
        default: if (value == 0 || value > 0x8000) return false; break;
        }
        // On a rip site the text is a cross-check: the value must still be exactly
        // that literal. On a schema site it is the field name, used to find the
        // declaration in the first place, and says nothing about the offset.
        if (site.kind == SiteKind::Rip && site.text) {
            const auto length = strlen(site.text) + 1;
            // A wrong decode can land just short of the end of the image, so the
            // whole comparison has to fit inside it and not only its first byte.
            if (value + length > header_.OptionalHeader.SizeOfImage) return false;
            if (memcmp(At(value), site.text, length) != 0) return false;
        }
        return true;
    }

    bool ResolveSite(const Site& site, Resolution& result) {
        if (site.kind == SiteKind::Schema) return ResolveSchemaSite(site, result);
        const uintptr_t function = *site.function;
        const auto pattern = detail::MakePattern(site.bytes, site.mask, site.size);
        uintptr_t value = 0;
        bool found = false, ambiguous = false, searched = false;
        const uintptr_t hint = function + site.offset;
        if (InText(hint) && detail::MatchesAt(At(hint), pattern) && Accept(site, At(hint), hint, value)) found = true;
        if (!found) {
            // Search the function that contains it. Windows carry enough context
            // to be unique there; where they are not, every match must agree.
            searched = true;
            const uintptr_t end = FunctionEnd(function);
            detail::ForEachMatch(At(function), end > function ? end - function : 0, pattern, [&](size_t at) {
                uintptr_t candidate = 0;
                if (!Accept(site, At(function + at), function + at, candidate)) return true;
                if (found && candidate != value) { ambiguous = true; return false; }
                value = candidate;
                found = true;
                return true;
            });
        }
        if (!found || ambiguous) {
            Fail(result, site.name, ambiguous ? "site is ambiguous" : "site not found");
            return false;
        }
        // A call that only repeats what a signature already proved unique across
        // the whole of .text is the weaker anchor of the two. It is kept as a
        // cross-check: it may confirm that address, never quietly replace it.
        if (site.verify) {
            if (*site.target != value) {
                Fail(result, site.name, "call site disagrees with the signature");
                return false;
            }
            return true;
        }
        *site.target = value;
        if (searched) { ++result.moved; ++result.searched; }
        return true;
    }

    // Field offsets are declared to Source 2's schema system next to the field
    // name, which survives updates even when the surrounding code moves.
    bool ResolveSchemaSite(const Site& site, Resolution& result) {
        const auto pattern = detail::MakePattern(site.bytes, site.mask, site.size);
        uintptr_t value = 0;
        if (InText(site.offset) && detail::MatchesAt(At(site.offset), pattern) && Accept(site, At(site.offset), site.offset, value)) {
            *site.target = value;
            return true;
        }
        const uintptr_t name = FindString(site.text);
        if (!name) { Fail(result, site.name, "schema field name not found"); return false; }
        // The registration passes the offset just before the field name, so the
        // declaration nearest a reference to that name is the one that owns it.
        // Later references belong to other registrations of other fields, which
        // is why the first usable one decides rather than a vote between them.
        bool found = false;
        uintptr_t first = 0;
        ForEachStringReference(name, [&](uintptr_t reference) {
            const uintptr_t begin = reference > 0x40 ? reference - 0x40 : 0;
            for (uintptr_t at = reference; at-- > begin;) {
                if (!InText(at) || !detail::MatchesAt(At(at), pattern)) continue;
                uintptr_t candidate = 0;
                if (!Accept(site, At(at), at, candidate)) continue;
                first = candidate;
                found = true;
                return false;
            }
            return true;
        });
        if (!found) {
            Fail(result, site.name, "schema field not declared");
            return false;
        }
        *site.target = first;
        ++result.moved;
        ++result.searched;
        return true;
    }

    uintptr_t FindString(const char* text) const {
        const size_t length = strlen(text);
        for (uint16_t i = 0; i < sectionCount_; ++i) {
            const auto& section = sections_[i];
            if (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
            const auto begin = At(section.VirtualAddress);
            const size_t size = section.Misc.VirtualSize;
            for (size_t at = 1; at + length + 1 <= size;) {
                const auto found = static_cast<const uint8_t*>(memchr(begin + at, text[0], size - length - at));
                if (!found) break;
                const size_t start = size_t(found - begin);
                if (found[-1] == 0 && !memcmp(found, text, length + 1)) return section.VirtualAddress + start;
                at = start + 1;
            }
        }
        return 0;
    }

    // lea/mov/cmp reg, [rip + disp]: the operand is measured from the next
    // instruction, so the encoding length decides where the target lands.
    template <class Visit>
    void ForEachStringReference(uintptr_t target, Visit visit) const {
        for (size_t at = 0; at + 7 <= textSize_; ++at) {
            const uint8_t opcode = text_[at];
            if (opcode != 0x8D && opcode != 0x8B && opcode != 0x89 && opcode != 0x3B && opcode != 0x39 && opcode != 0x83) continue;
            if ((text_[at + 1] & 0xC7) != 0x05) continue;
            int32_t displacement = 0;
            memcpy(&displacement, text_ + at + 2, 4);
            const uintptr_t reached = textRva_ + at + 6 + (opcode == 0x83 ? 1 : 0) + displacement;
            if (reached == target && !visit(textRva_ + at)) return;
        }
    }
};

}  // namespace appearance::profile
