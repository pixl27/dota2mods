#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

// A dependency-free PNG writer, used by --capture so the window can be saved as
// an image for a bug report or a design review. Deflate has a "stored" block
// type that copies bytes verbatim, so a valid PNG needs no compressor at all.
namespace wardrobe::capture {

inline uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) value = (value & 1) ? 0xEDB88320u ^ (value >> 1) : value >> 1;
            table[i] = value;
        }
        ready = true;
    }
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

inline void PushBig(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value >> 24));
    out.push_back(uint8_t(value >> 16));
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}

inline void PushChunk(std::vector<uint8_t>& out, const char (&type)[5], const std::vector<uint8_t>& body) {
    PushBig(out, uint32_t(body.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    PushBig(out, Crc32(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
}

// pixels is tightly packed RGB, top row first.
inline bool WritePng(const char* path, const uint8_t* pixels, uint32_t width, uint32_t height) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 3 + 1));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);   // filter: none
        raw.insert(raw.end(), pixels + size_t(y) * width * 3, pixels + size_t(y + 1) * width * 3);
    }

    std::vector<uint8_t> deflated{0x78, 0x01};   // zlib header, no preset dictionary
    for (size_t at = 0; at < raw.size();) {
        const uint16_t take = uint16_t(std::min<size_t>(65535, raw.size() - at));
        const bool last = at + take >= raw.size();
        deflated.push_back(last ? 1 : 0);
        deflated.push_back(uint8_t(take));
        deflated.push_back(uint8_t(take >> 8));
        deflated.push_back(uint8_t(~take));
        deflated.push_back(uint8_t(~take >> 8));
        deflated.insert(deflated.end(), raw.begin() + at, raw.begin() + at + take);
        at += take;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) { a = (a + byte) % 65521; b = (b + a) % 65521; }
    PushBig(deflated, (b << 16) | a);

    std::vector<uint8_t> file{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> header;
    PushBig(header, width);
    PushBig(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0});   // 8 bits per channel, truecolour
    PushChunk(file, "IHDR", header);
    PushChunk(file, "IDAT", deflated);
    PushChunk(file, "IEND", {});

    FILE* out = nullptr;
    if (fopen_s(&out, path, "wb") != 0 || !out) return false;
    const bool ok = fwrite(file.data(), 1, file.size(), out) == file.size();
    fclose(out);
    return ok;
}

}  // namespace wardrobe::capture
