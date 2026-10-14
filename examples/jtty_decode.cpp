// jtty_decode <recording.wav> [--json]
//
// Decodes every JTTY message in a mono 12 kHz signed 16-bit WAV file of at most
// 120 s. Prints one line per message: <start seconds> <frequency Hz> <text>.
// Exit status: 0 success (including "nothing decoded"), 2 on any error.
#include "wav.hpp"

#include <jtty/receiver.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::strcmp(argv[2], "--json") != 0)) {
        std::fprintf(stderr, "usage: jtty_decode <recording.wav> [--json]\n");
        return 2;
    }
    const bool json = argc == 3;
    std::vector<float> samples;
    std::string error;
    if (!wav::read(argv[1], samples, error)) {
        std::fprintf(stderr, "jtty_decode: %s\n", error.c_str());
        return 2;
    }
    const auto report = jtty::receive_messages(samples);
    if (!report) {
        std::fprintf(stderr, "jtty_decode: %s\n", report.error.c_str());
        return 2;
    }
    if (json) std::printf("[");
    bool first = true;
    for (const auto& message : report.value.results) {
        const double seconds = static_cast<double>(message.start_sample_12000) / jtty::sample_rate_hz;
        if (json) {
            std::string escaped;
            for (const char c : message.text) {
                if (c == '"' || c == '\\') escaped += '\\';
                escaped += c;
            }
            std::printf("%s{\"start_seconds\":%.3f,\"frequency_hz\":%d,\"text\":\"%s\"}", first ? "" : ",", seconds,
                        message.frequency_hz, escaped.c_str());
        } else {
            std::printf("%.3f %d %s\n", seconds, message.frequency_hz, message.text.c_str());
        }
        first = false;
    }
    if (json) std::printf("]\n");
    return 0;
}
