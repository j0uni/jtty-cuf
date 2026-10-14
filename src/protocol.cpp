#include "jtty/protocol.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

namespace jtty {
namespace {

constexpr std::string_view alphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?!\"#$%,&*()_'=[]{}<>|:;";
static_assert(alphabet.size() == 64);
constexpr std::array<std::uint8_t, 13> sync_tones = {0, 2, 2, 3, 0, 0, 3, 2, 1, 3, 1, 2, 0};
constexpr unsigned max_pcm_samples = sample_rate_hz * 10;
constexpr std::size_t max_waveform_frames = 16;

unsigned parity(unsigned value) { return std::popcount(value) & 1U; }

std::uint16_t crc_step(std::uint16_t reg, std::uint8_t bit) {
    reg ^= static_cast<std::uint16_t>(bit) << 11U;
    reg = (reg & 0x800U) != 0U ? static_cast<std::uint16_t>((reg << 1U) ^ 0x80FU)
                              : static_cast<std::uint16_t>(reg << 1U);
    return reg & 0xFFFU;
}

std::string normalize(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    bool pending_space = false;
    for (const unsigned char raw : input) {
        if (raw == ' ') {
            pending_space = !output.empty();
            continue;
        }
        char ch = static_cast<char>(raw);
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        if (alphabet.find(ch) == std::string_view::npos) ch = '#';
        if (pending_space) {
            output.push_back(' ');
            pending_space = false;
        }
        output.push_back(ch);
    }
    return output;
}

std::uint16_t next_state(std::uint16_t state, std::uint8_t bit) {
    return static_cast<std::uint16_t>(((state << 1U) | bit) & 0x1FFU);
}

std::array<std::uint8_t, 2> code_output(std::uint16_t state, std::uint8_t bit) {
    const unsigned reg = (static_cast<unsigned>(state) << 1U) | bit;
    return {static_cast<std::uint8_t>(parity(reg & 01167U)),
            static_cast<std::uint8_t>(parity(reg & 01545U))};
}

Result<EncodedFrame> frame_from_information(const InfoBits& information) {
    std::uint16_t remainder = 0;
    for (const auto bit : information) remainder = crc_step(remainder, bit);
    if (remainder != 0) return {.error = "CRC-12 validation failed"};

    EncodedFrame frame;
    frame.information = information;
    std::copy_n(information.begin(), frame.payload.size(), frame.payload.begin());
    std::copy(information.begin() + 34, information.end(), frame.crc.begin());
    if (frame.payload[32] != 0) return {.error = "reserved payload bit is set"};
    if (frame.payload[30] != 1 || frame.payload[31] != 1) {
        return {.error = "payload is not a TEXT5 atom"};
    }
    if (frame.payload[33] != 1) return {.error = "frame is not final"};

    std::uint32_t word = 0;
    std::string text;
    for (unsigned character = 0; character < 5; ++character) {
        unsigned index = 0;
        for (unsigned bit = 0; bit < 6; ++bit) {
            index = (index << 1U) | frame.payload[character * 6U + bit];
        }
        word = (word << 6U) | index;
        text.push_back(alphabet[index]);
    }
    word = (word << 2U) | 3U;
    while (!text.empty() && text.back() == ' ') text.pop_back();
    if (text.empty()) return {.error = "decoded text is empty"};
    frame.canonical_text = std::move(text);
    frame.message_word = word;
    frame.end_of_message = true;
    return {.value = std::move(frame)};
}

double symbol_energy(std::span<const float> samples, double frequency_hz) {
    const double omega = 2.0 * std::numbers::pi * frequency_hz / sample_rate_hz;
    double real = 0;
    double imag = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const double phase = omega * static_cast<double>(i);
        real += static_cast<double>(samples[i]) * std::cos(phase);
        imag -= static_cast<double>(samples[i]) * std::sin(phase);
    }
    return real * real + imag * imag;
}

}  // namespace

CompatibilityId compatibility_id() {
    return {.protocol_name = "JTTY",
            .behavior_revision = "wsjtx-3.3.0-beta1-first-slice-v1",
            .reference_release = "WSJT-X 3.3.0-beta1",
            .reference_commit = "57ba9337fa020065dc0ceee2c4ef85e433ea32d4"};
}

Result<EncodedFrame> encode_literal(std::string_view text) {
    if (text.size() > 4096) return {.error = "literal input exceeds 4096-byte bound"};
    const auto canonical = normalize(text);
    if (canonical.empty() || canonical.size() > 5) {
        return {.error = "a single-frame literal must normalize to one through five characters"};
    }
    EncodedFrame frame;
    frame.canonical_text = canonical;
    frame.end_of_message = true;
    std::string padded = canonical;
    padded.resize(5, ' ');
    std::size_t offset = 0;
    std::uint32_t word = 0;
    for (const char ch : padded) {
        const auto index = static_cast<unsigned>(alphabet.find(ch));
        word = (word << 6U) | index;
        for (int bit = 5; bit >= 0; --bit) {
            frame.payload[offset++] = static_cast<std::uint8_t>((index >> bit) & 1U);
        }
    }
    frame.message_word = (word << 2U) | 3U;
    frame.payload[30] = 1;
    frame.payload[31] = 1;
    frame.payload[32] = 0;
    frame.payload[33] = 1;
    std::copy(frame.payload.begin(), frame.payload.end(), frame.information.begin());
    std::uint16_t remainder = 0;
    for (const auto bit : frame.payload) remainder = crc_step(remainder, bit);
    for (unsigned bit = 0; bit < 12; ++bit) {
        const auto value = static_cast<std::uint8_t>((remainder >> (11U - bit)) & 1U);
        frame.crc[bit] = value;
        frame.information[34U + bit] = value;
    }

    std::uint16_t state = 0;
    for (unsigned i = 37; i < 46; ++i) state = next_state(state, frame.information[i]);
    for (unsigned i = 0; i < frame.information.size(); ++i) {
        const auto output = code_output(state, frame.information[i]);
        frame.coded[2U * i] = output[0];
        frame.coded[2U * i + 1U] = output[1];
        state = next_state(state, frame.information[i]);
    }
    std::copy(sync_tones.begin(), sync_tones.end(), frame.tones.begin());
    constexpr std::array<std::uint8_t, 4> pair_to_tone = {0, 1, 3, 2};
    for (unsigned i = 0; i < 46; ++i) {
        frame.tones[13U + i] = pair_to_tone[(frame.coded[2U * i] << 1U) |
                                             frame.coded[2U * i + 1U]];
    }
    return {.value = std::move(frame)};
}

Result<EncodedFrame> decode_coded_bits(const CodedBits& bits) {
    if (std::any_of(bits.begin(), bits.end(), [](std::uint8_t bit) { return bit > 1; })) {
        return {.error = "coded bits must contain only 0 or 1"};
    }
    constexpr unsigned infinity = std::numeric_limits<unsigned>::max() / 4U;
    unsigned best_metric = infinity;
    InfoBits best{};
    bool found = false;
    for (unsigned initial = 0; initial < 512; ++initial) {
        std::array<unsigned, 512> current{};
        current.fill(infinity);
        current[initial] = 0;
        std::array<std::array<std::uint16_t, 512>, 46> previous{};
        std::array<std::array<std::uint8_t, 512>, 46> chosen{};
        for (unsigned step = 0; step < 46; ++step) {
            std::array<unsigned, 512> next{};
            next.fill(infinity);
            for (unsigned state = 0; state < 512; ++state) {
                if (current[state] == infinity) continue;
                for (std::uint8_t bit = 0; bit <= 1; ++bit) {
                    const auto output = code_output(static_cast<std::uint16_t>(state), bit);
                    const unsigned metric = current[state] + (output[0] != bits[2U * step]) +
                                            (output[1] != bits[2U * step + 1U]);
                    const auto destination = next_state(static_cast<std::uint16_t>(state), bit);
                    if (metric < next[destination]) {
                        next[destination] = metric;
                        previous[step][destination] = static_cast<std::uint16_t>(state);
                        chosen[step][destination] = bit;
                    }
                }
            }
            current = next;
        }
        if (current[initial] >= best_metric) continue;
        InfoBits candidate{};
        std::uint16_t state = static_cast<std::uint16_t>(initial);
        for (int step = 45; step >= 0; --step) {
            candidate[static_cast<unsigned>(step)] = chosen[static_cast<unsigned>(step)][state];
            state = previous[static_cast<unsigned>(step)][state];
        }
        if (frame_from_information(candidate)) {
            best_metric = current[initial];
            best = candidate;
            found = true;
        }
    }
    if (!found) return {.error = "no valid tail-biting codeword"};
    auto result = frame_from_information(best);
    result.value.coded = bits;
    std::copy(sync_tones.begin(), sync_tones.end(), result.value.tones.begin());
    constexpr std::array<std::uint8_t, 4> pair_to_tone = {0, 1, 3, 2};
    for (unsigned i = 0; i < 46; ++i) {
        result.value.tones[13U + i] = pair_to_tone[(bits[2U * i] << 1U) | bits[2U * i + 1U]];
    }
    return result;
}

Result<std::vector<float>> modulate(const ToneFrame& tones, double base_frequency_hz,
                                    double amplitude) {
    return modulate_frames(std::span(&tones, 1), base_frequency_hz, amplitude);
}

namespace {

// One continuous-phase Gaussian synthesis, parameterized only by the output
// sample rate. The symbol (rate / 31.25), ramp (one eighth symbol), and phase
// increment all scale with the rate, so every rate renders the same public
// formulas directly; no resampling is involved.
Result<std::vector<float>> synthesize(std::span<const ToneFrame> frames,
                                      double base_frequency_hz, double amplitude,
                                      unsigned output_rate_hz) {
    if (frames.empty() || frames.size() > max_waveform_frames) {
        return {.error = "frame count is outside the 1..16 waveform bound"};
    }
    if (!std::isfinite(base_frequency_hz) ||
        base_frequency_hz < static_cast<double>(min_base_frequency_hz) ||
        base_frequency_hz > static_cast<double>(max_base_frequency_hz) ||
        std::trunc(base_frequency_hz) != base_frequency_hz) {
        return {.error = "base frequency must be an integer from 200 through 5000 Hz"};
    }
    if (!std::isfinite(amplitude) || amplitude <= 0.0 || amplitude > 1.0) {
        return {.error = "amplitude must be finite and in (0, 1]"};
    }
    if (std::any_of(frames.begin(), frames.end(), [](const ToneFrame& tones) {
            return std::any_of(tones.begin(), tones.end(),
                               [](std::uint8_t tone) { return tone > 3; });
        })) {
        return {.error = "tone index exceeds 3"};
    }
    const std::size_t symbol_samples =
        static_cast<std::size_t>(samples_per_symbol) * (output_rate_hz / sample_rate_hz);
    std::vector<float> output(frames.size() * symbols_per_frame * symbol_samples);
    const double c = std::numbers::pi * std::sqrt(2.0 / std::log(2.0));
    const auto pulse = [c](double t) {
        return 0.5 * (std::erf(c * 2.0 * (t + 0.5)) - std::erf(c * 2.0 * (t - 0.5)));
    };
    const std::size_t total_symbols = frames.size() * symbols_per_frame;
    const auto tone_at = [frames, total_symbols](std::int64_t index) -> double {
        if (index < 0) return frames.front().front();
        if (static_cast<std::size_t>(index) >= total_symbols) return frames.back().back();
        const auto absolute = static_cast<std::size_t>(index);
        return frames[absolute / symbols_per_frame][absolute % symbols_per_frame];
    };
    const std::size_t ramp = symbol_samples / 8U;
    const double rate = static_cast<double>(output_rate_hz);
    double phase = 0;
    for (std::size_t n = 0; n < output.size(); ++n) {
        const std::size_t symbol = n / symbol_samples;
        const double local = (static_cast<double>(n % symbol_samples) + 0.5) /
                                 static_cast<double>(symbol_samples) - 0.5;
        const auto signed_symbol = static_cast<std::int64_t>(symbol);
        const double shaped = tone_at(signed_symbol - 1) * pulse(local + 1.0) +
                              tone_at(signed_symbol) * pulse(local) +
                              tone_at(signed_symbol + 1) * pulse(local - 1.0);
        double envelope = 1.0;
        if (n < ramp) {
            envelope = 0.5 * (1.0 - std::cos(std::numbers::pi * static_cast<double>(n) /
                                              static_cast<double>(ramp)));
        } else if (n >= output.size() - ramp) {
            const auto remaining = output.size() - 1U - n;
            envelope = 0.5 * (1.0 - std::cos(std::numbers::pi * static_cast<double>(remaining) /
                                              static_cast<double>(ramp)));
        }
        output[n] = static_cast<float>(amplitude * envelope * std::sin(phase));
        phase = std::remainder(phase + 2.0 * std::numbers::pi *
                                  (base_frequency_hz + tone_spacing_hz * shaped) / rate,
                               2.0 * std::numbers::pi);
    }
    return {.value = std::move(output)};
}

}  // namespace

Result<std::vector<float>> modulate_frames(std::span<const ToneFrame> frames,
                                           double base_frequency_hz, double amplitude) {
    return synthesize(frames, base_frequency_hz, amplitude, sample_rate_hz);
}

Result<std::vector<float>> modulate_frames_48k(std::span<const ToneFrame> frames,
                                               double base_frequency_hz, double amplitude) {
    return synthesize(frames, base_frequency_hz, amplitude, application_sample_rate_hz);
}

Result<DecodedFrame> decode_clean_frame(std::span<const float> pcm_12k,
                                        std::uint64_t start_sample_12000) {
    if (pcm_12k.size() < samples_per_frame || pcm_12k.size() > max_pcm_samples) {
        return {.error = "PCM length is outside the bounded single-frame window"};
    }
    if (!std::all_of(pcm_12k.begin(), pcm_12k.end(),
                     [](float value) { return std::isfinite(value); })) {
        return {.error = "PCM contains a non-finite sample"};
    }
    int best_base = 1450;
    double best_sync = -1;
    for (int base = 1450; base <= 1650; ++base) {
        double score = 0;
        for (unsigned symbol = 0; symbol < sync_tones.size(); ++symbol) {
            const auto window = pcm_12k.subspan(symbol * samples_per_symbol + 64, 256);
            const double expected = symbol_energy(window, base + sync_tones[symbol] * tone_spacing_hz);
            double alternatives = 0;
            for (unsigned tone = 0; tone < 4; ++tone) {
                if (tone != sync_tones[symbol]) {
                    alternatives = std::max(alternatives,
                        symbol_energy(window, base + tone * tone_spacing_hz));
                }
            }
            score += expected - alternatives;
        }
        if (score > best_sync) {
            best_sync = score;
            best_base = base;
        }
    }
    if (!(best_sync > 0)) return {.error = "synchronization word not found"};

    constexpr std::array<std::array<std::uint8_t, 2>, 4> tone_to_pair =
        {{{0, 0}, {0, 1}, {1, 1}, {1, 0}}};
    CodedBits coded{};
    for (unsigned data = 0; data < 46; ++data) {
        const unsigned symbol = 13U + data;
        const auto window = pcm_12k.subspan(symbol * samples_per_symbol + 64, 256);
        unsigned best_tone = 0;
        double best_energy = -1;
        for (unsigned tone = 0; tone < 4; ++tone) {
            const double energy = symbol_energy(window, best_base + tone * tone_spacing_hz);
            if (energy > best_energy) {
                best_energy = energy;
                best_tone = tone;
            }
        }
        coded[2U * data] = tone_to_pair[best_tone][0];
        coded[2U * data + 1U] = tone_to_pair[best_tone][1];
    }
    const auto decoded = decode_coded_bits(coded);
    if (!decoded) return {.error = decoded.error};
    return {.value = {.text = decoded.value.canonical_text,
                      .complete = true,
                      .start_sample_12000 = start_sample_12000,
                      .frequency_hz = best_base}};
}

}  // namespace jtty
