// Minimal reader and writer for mono 12 kHz signed 16-bit PCM WAV files.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace wav {

// Reads `path` into samples scaled to [-1, 1). On failure returns false and sets `error`.
inline bool read(const std::string& path, std::vector<float>& samples, std::string& error) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) { error = "cannot open " + path; return false; }
    std::vector<unsigned char> raw;
    unsigned char buffer[65536];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
        raw.insert(raw.end(), buffer, buffer + n);
        if (raw.size() > 8'000'000) { std::fclose(file); error = "file is too large"; return false; }
    }
    std::fclose(file);
    const auto u16 = [&](std::size_t at) { return static_cast<unsigned>(raw[at]) | (static_cast<unsigned>(raw[at + 1]) << 8); };
    const auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16); };
    if (raw.size() < 44 || std::string(raw.begin(), raw.begin() + 4) != "RIFF" ||
        std::string(raw.begin() + 8, raw.begin() + 12) != "WAVE") {
        error = "not a RIFF/WAVE file";
        return false;
    }
    bool format_ok = false;
    std::size_t data_at = 0, data_size = 0;
    for (std::size_t at = 12; at + 8 <= raw.size();) {
        const std::string tag(raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at) + 4);
        const std::size_t size = u32(at + 4);
        at += 8;
        if (size > raw.size() - at) { error = "truncated WAV chunk"; return false; }
        if (tag == "fmt ") {
            if (size < 16 || u16(at) != 1 || u16(at + 2) != 1 || u32(at + 4) != 12000 || u16(at + 14) != 16) {
                error = "unsupported format: mono 12 kHz signed 16-bit PCM is required";
                return false;
            }
            format_ok = true;
        } else if (tag == "data") {
            data_at = at;
            data_size = size;
        }
        at += size + (size & 1U);
    }
    if (!format_ok || data_size < 2) { error = "WAV lacks a valid fmt or data chunk"; return false; }
    samples.resize(data_size / 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(static_cast<std::int16_t>(u16(data_at + 2 * i))) / 32768.0F;
    }
    return true;
}

inline bool write(const std::string& path, const std::vector<float>& samples) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const std::uint32_t bytes = static_cast<std::uint32_t>(samples.size() * 2);
    const std::uint32_t riff = 36U + bytes, fmt = 16U, rate = 12000U, byte_rate = 24000U;
    const std::uint16_t pcm = 1, channels = 1, align = 2, bits = 16;
    std::fwrite("RIFF", 1, 4, file); std::fwrite(&riff, 4, 1, file); std::fwrite("WAVEfmt ", 1, 8, file);
    std::fwrite(&fmt, 4, 1, file); std::fwrite(&pcm, 2, 1, file); std::fwrite(&channels, 2, 1, file);
    std::fwrite(&rate, 4, 1, file); std::fwrite(&byte_rate, 4, 1, file); std::fwrite(&align, 2, 1, file);
    std::fwrite(&bits, 2, 1, file); std::fwrite("data", 1, 4, file); std::fwrite(&bytes, 4, 1, file);
    for (const float v : samples) {
        float scaled = v * 32768.0F;
        if (scaled > 32767.0F) scaled = 32767.0F;
        if (scaled < -32768.0F) scaled = -32768.0F;
        const std::int16_t q = static_cast<std::int16_t>(scaled);
        std::fwrite(&q, 2, 1, file);
    }
    return std::fclose(file) == 0;
}

}  // namespace wav
