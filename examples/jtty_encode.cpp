// jtty_encode "<text>" <out.wav> [base_frequency_hz]
//
// Renders a JTTY message (up to 80 characters, 1 to 16 frames) as a mono 12 kHz
// signed 16-bit WAV file. The base frequency is the lowest tone, 200 to 5000 Hz
// (default 1500).
// Exit status: 0 success, 2 on any error.
#include "wav.hpp"

#include <jtty/message_codec.hpp>
#include <jtty/protocol.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr, "usage: jtty_encode \"<text>\" <out.wav> [base_frequency_hz]\n");
        return 2;
    }
    const double frequency = argc == 4 ? std::atof(argv[3]) : jtty::default_base_frequency_hz;
    const auto message = jtty::pack_literal_message(argv[1], jtty::MessageProfile::unknown);
    if (!message) {
        std::fprintf(stderr, "jtty_encode: %s\n", message.error.c_str());
        return 2;
    }
    const auto audio = jtty::modulate_message(message.value.frames, frequency, 0.5);
    if (!audio) {
        std::fprintf(stderr, "jtty_encode: %s\n", audio.error.c_str());
        return 2;
    }
    // Half a second of silence before and after, as a recording would have.
    std::vector<float> samples(6000, 0.0F);
    samples.insert(samples.end(), audio.value.begin(), audio.value.end());
    samples.resize(samples.size() + 6000, 0.0F);
    if (!wav::write(argv[2], samples)) {
        std::fprintf(stderr, "jtty_encode: cannot write %s\n", argv[2]);
        return 2;
    }
    std::printf("%s -> %s (%zu frame%s)\n", message.value.canonical_text.c_str(), argv[2], message.value.frames.size(),
                message.value.frames.size() == 1 ? "" : "s");
    return 0;
}
