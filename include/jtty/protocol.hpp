#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jtty {

struct CompatibilityId {
    std::string protocol_name;
    std::string behavior_revision;
    std::string reference_release;
    std::string reference_commit;
};

inline constexpr unsigned sample_rate_hz = 12'000;
inline constexpr unsigned symbols_per_frame = 59;
inline constexpr unsigned samples_per_symbol = 384;
inline constexpr unsigned samples_per_frame = symbols_per_frame * samples_per_symbol;
inline constexpr double tone_spacing_hz = 31.25;
// Application transmit-path rate: the same symbols rendered directly at 48 kHz
// (1,536 samples per symbol, 192-sample edge ramp).
inline constexpr unsigned application_sample_rate_hz = 48'000;
inline constexpr unsigned application_samples_per_symbol =
    samples_per_symbol * (application_sample_rate_hz / sample_rate_hz);
inline constexpr unsigned application_samples_per_frame =
    symbols_per_frame * application_samples_per_symbol;
// Transmit base audio frequency (nominal tone 0), integer hertz.
inline constexpr int min_base_frequency_hz = 200;
inline constexpr int max_base_frequency_hz = 5000;
inline constexpr int default_base_frequency_hz = 1500;

using PayloadBits = std::array<std::uint8_t, 34>;
using InfoBits = std::array<std::uint8_t, 46>;
using CodedBits = std::array<std::uint8_t, 92>;
using ToneFrame = std::array<std::uint8_t, 59>;

struct EncodedFrame {
    std::string canonical_text;
    std::uint32_t message_word{};
    bool end_of_message{};
    PayloadBits payload{};
    std::array<std::uint8_t, 12> crc{};
    InfoBits information{};
    CodedBits coded{};
    ToneFrame tones{};
};

struct DecodedFrame {
    std::string text;
    bool complete{};
    std::uint64_t start_sample_12000{};
    int frequency_hz{};
};

template <typename T>
struct Result {
    T value{};
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] CompatibilityId compatibility_id();
[[nodiscard]] Result<EncodedFrame> encode_literal(std::string_view text);
[[nodiscard]] Result<EncodedFrame> decode_coded_bits(const CodedBits& bits);
// base_frequency_hz must be an integer from min_base_frequency_hz through
// max_base_frequency_hz; any other value is rejected, never clamped.
[[nodiscard]] Result<std::vector<float>> modulate(const ToneFrame& tones,
                                                  double base_frequency_hz,
                                                  double amplitude);
// Generates one bounded, gapless waveform for one through sixteen frames.
// Phase and Gaussian pulse shaping continue across frame boundaries and the
// raised-cosine envelope is applied only at the complete waveform edges.
[[nodiscard]] Result<std::vector<float>> modulate_frames(
    std::span<const ToneFrame> frames, double base_frequency_hz, double amplitude);
// Same waveform synthesized directly at application_sample_rate_hz from the
// public formulas (no 12 kHz intermediate and no resampling). Validation and
// bounds are identical to modulate_frames.
[[nodiscard]] Result<std::vector<float>> modulate_frames_48k(
    std::span<const ToneFrame> frames, double base_frequency_hz, double amplitude);
[[nodiscard]] Result<DecodedFrame> decode_clean_frame(
    std::span<const float> pcm_12k, std::uint64_t start_sample_12000 = 0);

}  // namespace jtty
