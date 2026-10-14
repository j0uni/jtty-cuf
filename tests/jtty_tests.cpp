// Basic tests of the JTTY library: encode a message, render it as audio, add
// noise or a second signal, and check what the receiver decodes. No external
// test framework and no WSJT-X involved.
#include <jtty/message_codec.hpp>
#include <jtty/protocol.hpp>
#include <jtty/receiver.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                                   \
        }                                                                                 \
    } while (false)

// Deterministic Gaussian noise (Box-Muller over a fixed generator) so results do not
// depend on the standard library's distribution implementation.
class Noise {
public:
    explicit Noise(std::uint64_t seed) : rng_(seed) {}
    double next() {
        constexpr double two_pi = 6.283185307179586;
        if (have_spare_) { have_spare_ = false; return spare_; }
        const double u1 = (static_cast<double>(rng_() >> 11) + 1.0) / 9007199254740993.0;
        const double u2 = static_cast<double>(rng_() >> 11) / 9007199254740992.0;
        const double r = std::sqrt(-2.0 * std::log(u1));
        spare_ = r * std::sin(two_pi * u2);
        have_spare_ = true;
        return r * std::cos(two_pi * u2);
    }

private:
    std::mt19937_64 rng_;
    bool have_spare_ = false;
    double spare_ = 0;
};

std::vector<float> render(const std::string& text, double hz, double amplitude) {
    const auto message = jtty::pack_literal_message(text, jtty::MessageProfile::unknown);
    CHECK(message);
    const auto audio = jtty::modulate_message(message.value.frames, hz, amplitude);
    CHECK(audio);
    return audio.value;
}

// Puts `signal` into a recording of `total` samples starting at `start` (may be negative).
void mix(std::vector<float>& recording, const std::vector<float>& signal, long start, double gain = 1.0) {
    for (std::size_t i = 0; i < signal.size(); ++i) {
        const long at = start + static_cast<long>(i);
        if (at >= 0 && at < static_cast<long>(recording.size())) {
            recording[static_cast<std::size_t>(at)] += static_cast<float>(gain * signal[i]);
        }
    }
}

// Signal gain that puts the signal at `snr_db` over the noise in a 2500 Hz bandwidth.
double gain_for_snr(const std::vector<float>& signal, double snr_db, double noise_rms) {
    double power = 0;
    for (const float v : signal) power += static_cast<double>(v) * static_cast<double>(v);
    power /= static_cast<double>(signal.size());
    const double noise_in_band = noise_rms * noise_rms / 6000.0 * 2500.0;
    return std::sqrt(std::pow(10.0, snr_db / 10.0) * noise_in_band / power);
}

void add_noise(std::vector<float>& recording, double rms, std::uint64_t seed) {
    Noise noise(seed);
    for (auto& v : recording) v += static_cast<float>(rms * noise.next());
}

bool decoded(const jtty::ReceiveReport& report, const std::string& text, int hz, int tolerance_hz = 3) {
    return std::any_of(report.results.begin(), report.results.end(), [&](const jtty::ReceivedMessage& m) {
        return m.text == text && std::abs(m.frequency_hz - hz) <= tolerance_hz;
    });
}

void test_clean_round_trips() {
    // One- to five-frame messages across the supported range; the reported start
    // and frequency match what was transmitted.
    const std::vector<std::string> texts = {"HELLO", "K1ABC HELLO", "CQ K1ABC FN42", "HELLO, WORLD! 73 DE K1ABC"};
    for (const double hz : {300.0, 1500.0, 2800.0}) {
        for (const auto& text : texts) {
            const auto signal = render(text, hz, 0.5);
            auto recording = std::vector<float>(signal.size() + 20'000, 0.0F);
            mix(recording, signal, 7000);
            const auto report = jtty::receive_messages(recording);
            CHECK(report);
            CHECK(report.value.results.size() == 1);
            CHECK(decoded(report.value, text, static_cast<int>(hz)));
            if (!report.value.results.empty()) {
                CHECK(std::abs(report.value.results[0].start_sample_12000 - 7000) < 100);
            }
        }
    }
}

void test_longest_message() {
    // 80 characters spread over many frames.
    const std::string text = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 THE QUICK BROWN FOX JUMPS";
    const auto packed = jtty::pack_literal_message(text, jtty::MessageProfile::unknown);
    CHECK(packed);
    CHECK(packed.value.frames.size() >= 10 && packed.value.frames.size() <= 16);
    std::vector<float> recording(jtty::samples_per_frame * packed.value.frames.size() + 30'000, 0.0F);
    mix(recording, render(text, 1200.0, 0.5), 5000);
    const auto report = jtty::receive_messages(recording);
    CHECK(report);
    CHECK(decoded(report.value, text, 1200));
}

void test_noisy_recordings() {
    // About -10 dB (signal power against noise in 2500 Hz): well above the
    // receiver's limit of roughly -15 dB, so every trial must decode.
    const std::string text = "K1ABC HELLO";
    const auto signal = render(text, 1500.0, 1.0);
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        std::vector<float> recording(signal.size() + 60'000, 0.0F);
        mix(recording, signal, 9000 + static_cast<long>(seed) * 1337, gain_for_snr(signal, -10.0, 0.1));
        add_noise(recording, 0.1, seed);
        const auto report = jtty::receive_messages(recording);
        CHECK(report);
        CHECK(decoded(report.value, text, 1500));
    }
}

void test_two_signals() {
    // Two stations at different frequencies, one stronger, partly overlapping in time.
    std::vector<float> recording(150'000, 0.0F);
    mix(recording, render("K1ABC HELLO", 1000.0, 0.5), 5000);
    mix(recording, render("QRZ? DE N0CALL", 1900.0, 0.2), 20'000);
    add_noise(recording, 0.02, 11);
    const auto report = jtty::receive_messages(recording);
    CHECK(report);
    CHECK(report.value.results.size() == 2);
    CHECK(decoded(report.value, "K1ABC HELLO", 1000));
    CHECK(decoded(report.value, "QRZ? DE N0CALL", 1900));
    // Results are ordered by start time.
    if (report.value.results.size() == 2) {
        CHECK(report.value.results[0].start_sample_12000 <= report.value.results[1].start_sample_12000);
    }
}

void test_recording_starts_inside_a_message() {
    // The first frame is cut off, so only the tail of the message can be decoded.
    const auto signal = render("K1ABC HELLO", 1500.0, 0.5);
    std::vector<float> recording(signal.begin() + 10'000, signal.end());
    recording.resize(recording.size() + 12'000, 0.0F);
    const auto report = jtty::receive_messages(recording);
    CHECK(report);
    CHECK(decoded(report.value, "HELLO", 1500));

    // A frame cut by less than about 0.25 s is still recovered completely.
    std::vector<float> slightly(signal.begin() + 2000, signal.end());
    slightly.resize(slightly.size() + 12'000, 0.0F);
    const auto whole = jtty::receive_messages(slightly);
    CHECK(whole);
    CHECK(decoded(whole.value, "K1ABC HELLO", 1500));
}

void test_noise_only_decodes_nothing() {
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        std::vector<float> recording(120'000, 0.0F);
        add_noise(recording, 0.1, 100 + seed);
        const auto report = jtty::receive_messages(recording);
        CHECK(report);
        CHECK(report.value.results.empty());
        CHECK(report.value.outcome == jtty::ReceiveOutcome::no_result);
    }
    // Digital silence too.
    const auto silent = jtty::receive_messages(std::vector<float>(50'000, 0.0F));
    CHECK(silent);
    CHECK(silent.value.results.empty());
}

void test_errors_are_reported() {
    CHECK(!jtty::receive_messages(std::vector<float>{}));
    CHECK(!jtty::receive_messages(std::vector<float>(jtty::receiver_max_input_samples + 1, 0.0F)));
    std::vector<float> bad(30'000, 0.0F);
    bad[1000] = std::nanf("");
    CHECK(!jtty::receive_messages(bad));
    CHECK(!jtty::pack_literal_message("", jtty::MessageProfile::unknown));
    // Text is normalized (upper case, single spaces) and cut to 80 characters.
    const auto longer = jtty::pack_literal_message(std::string(200, 'a'), jtty::MessageProfile::unknown);
    CHECK(longer && longer.value.canonical_text == std::string(80, 'A') && longer.value.frames.size() == 16);
    CHECK(!jtty::modulate_message(jtty::pack_literal_message("HELLO", jtty::MessageProfile::unknown).value.frames, 50.0, 0.5));
}

void test_deterministic_and_thread_safe() {
    std::vector<std::vector<float>> recordings;
    for (int i = 0; i < 4; ++i) {
        std::vector<float> recording(80'000, 0.0F);
        mix(recording, render(i % 2 ? "K1ABC HELLO" : "QRZ? DE N0CALL", 900.0 + 400.0 * i, 0.3), 4000 + 1500 * i);
        add_noise(recording, 0.1, 50 + static_cast<std::uint64_t>(i));
        recordings.push_back(std::move(recording));
    }
    std::vector<jtty::ReceiveReport> expected;
    for (const auto& recording : recordings) {
        const auto report = jtty::receive_messages(recording);
        CHECK(report);
        const auto again = jtty::receive_messages(recording);
        CHECK(again && again.value.results.size() == report.value.results.size());
        expected.push_back(report.value);
    }
    // The same receptions on four threads at once return the same results.
    std::vector<int> mismatches(recordings.size(), 0);
    std::vector<std::thread> pool;
    for (std::size_t t = 0; t < recordings.size(); ++t) {
        pool.emplace_back([&, t] {
            for (int repeat = 0; repeat < 2; ++repeat) {
                const auto report = jtty::receive_messages(recordings[t]);
                if (!report || report.value.results.size() != expected[t].results.size()) { ++mismatches[t]; continue; }
                for (std::size_t i = 0; i < expected[t].results.size(); ++i) {
                    if (report.value.results[i].text != expected[t].results[i].text ||
                        report.value.results[i].frequency_hz != expected[t].results[i].frequency_hz ||
                        report.value.results[i].start_sample_12000 != expected[t].results[i].start_sample_12000) {
                        ++mismatches[t];
                    }
                }
            }
        });
    }
    for (auto& thread : pool) thread.join();
    for (const int m : mismatches) CHECK(m == 0);
}

}  // namespace

int main() {
    test_clean_round_trips();
    test_longest_message();
    test_noisy_recordings();
    test_two_signals();
    test_recording_starts_inside_a_message();
    test_noise_only_decodes_nothing();
    test_errors_are_reported();
    test_deterministic_and_thread_safe();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("jtty tests passed\n");
    return 0;
}
