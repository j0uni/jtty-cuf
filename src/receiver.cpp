#include "jtty/receiver.hpp"

#include "jtty/message_codec.hpp"
#include "receiver_assembly.hpp"
#include "receiver_dsp.hpp"
#include "receiver_fec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <deque>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>

// Labels in comments ("D5", "D13.1", ...) are section labels of the design notes
// this receiver was written from (a prose description of the WSJT-X JTTY
// receive path: sync map, refinement, gate, soft metrics, list decoding,
// validation, subtraction, assembly).

namespace jtty {
namespace {

using detail::Cf;
using detail::FrameEntry;
using detail::MergeResult;
using detail::OpenMessage;
using detail::alias_df_hz;
using detail::continuation_residual_s;
using detail::frame_period_s;
using detail::stamp_df_hz;
using detail::stamp_dt_s;
using detail::stand_in;
using detail::step_period_s;
using Complex = std::complex<double>;

constexpr double two_pi = 2.0 * std::numbers::pi;

// ---------------------------------------------------------------- constants (D15)
constexpr std::array<std::uint8_t, 13> sync_tones = {0, 2, 2, 3, 0, 0, 3, 2, 1, 3, 1, 2, 0};
constexpr int sync_symbols = 13;
constexpr int data_symbols = 46;
constexpr double fs = sample_rate_hz;                                  // 12 kHz
constexpr double fd = 6000.0;                                           // analysis rate
constexpr int n_sym = 192;                                              // samples/symbol at fd
constexpr std::int64_t frame_12k = samples_per_frame;                   // 22,656
constexpr std::int64_t step_12k = frame_12k / 4;                        // Q = 5,664
constexpr std::int64_t window_12k = frame_12k + step_12k;               // 28,320
constexpr int window_fd = static_cast<int>(window_12k / 2);             // 14,160
constexpr std::size_t analytic_size = 32768;                            // NA
constexpr std::size_t baseband_size = analytic_size / 2;                // 16,384 at fd
constexpr std::size_t map_size = 8192;                                  // B
constexpr double fbin = fd / static_cast<double>(map_size);             // 0.732421875 Hz
constexpr int map_steps = 237;                                          // tau = 0..2832 step 12
constexpr int map_step_fd = 12;
constexpr int sync_length_fd = sync_symbols * n_sym;                    // 2,496
constexpr int frame_fd = 59 * n_sym;                                    // 11,328

// Analytic passband: 0 to 3400 Hz (the WSJT-X decoder keeps 0 to 3000 Hz,
// D3). Wider so the candidate's 200--3000 Hz search interval can hold all four
// tones of a frame whose tone 0 lies at 3000 Hz. A complex 6 kHz signal holds
// any 6 kHz span unambiguously, so 3000--3400 Hz aliases onto -3000..-2600 Hz
// where nothing else lies.
constexpr double analytic_passband_hz = 3400.0;

// Candidate framing of a finite input as one reception (D2): the window grid is
// anchored lead_12k samples before input sample 0 (480 samples plus one 5,664
// sample step, so the window phase relative to the input is the same as with a
// 480 sample lead). A frame whose start lies before the input, its first
// symbols truncated, is searched down to search_reach_12k samples before input
// sample 0; earlier starts are not searched. The reach follows the reference
// application on frames cut at the start of the recording: it decoded the whole
// message at 3,000 samples and only the tail frames from 3,500 samples
// (2026-10-14, hidden-window oracle). Zero samples follow the input so that the
// last complete frame inside the input is searchable by a full window.
constexpr std::int64_t lead_12k = 480 + step_12k;
constexpr std::int64_t search_reach_12k = 3250;
constexpr std::int64_t tail_12k = step_12k + 2 * n_sym;

// Exclusion half-sizes and picks (D6).
constexpr int exclude_bins_below = 13;
constexpr int exclude_bins_above = 15;
constexpr int exclude_steps_below = 7;
constexpr int exclude_steps_above = 9;
constexpr int vicinity_bins = 14;
constexpr std::size_t max_recorded_subtractions = 16;

// Gates (D8).
constexpr int rx_min_hits = 7;
constexpr double rx_min_rho_db = 4.6;
constexpr int fixed_min_hits = 9;
constexpr double fixed_min_rho_db = 5.0;

// Plausibility test (not part of the WSJT-X decoder): a CRC- and grammar-valid
// word is accepted only when the full-symbol energy at its tones exceeds the
// mean energy at the other three tones by this factor, over the data symbols.
// A transmitted frame scores about 5.8 at -12 dB and at least 2.8 at -14 dB; a
// word that passes the CRC by chance on noise scores mostly 1.4 to 3.0. The
// extension channels (outside the reference's 1200--1800 Hz band, eight of the
// ten fixed channels) see most of the chance passes and use the stricter value;
// the Rx and fixed channels use 2.0 so that sensitivity matches the reference.
constexpr double min_decoded_tone_ratio = 2.0;
constexpr double min_decoded_tone_ratio_extension = 3.0;

// Duplicate and assembly thresholds (D13).
constexpr double retry_timing_s = 0.1;

// Bound on retained frame entries; equal to the
// decoded-frame budget, so it is never the binding limit.
constexpr std::size_t max_frame_history = receiver_max_decoded_frames;

// ------------------------------------------------------------------ channels (D4)
struct Channel {
    int b_lo{};
    int b_hi{};
    double center_hz{};
    double half_width_hz{};
    bool rx{};
    unsigned picks{};
    bool extension{};  // candidate extension channel: refined and gated like Rx
};

int bin_floor(double hz) { return static_cast<int>(hz / fbin); }

// Channel Rx and fixed channels Lo (1200--1500 Hz) and Hi
// (1500--1800 Hz), followed by candidate extension channels that tile the rest
// of the candidate's 200--3000 Hz search interval. Extension channels pick twice
// per pass like the fixed channels but are refined and gated like Rx (the
// WSJT-X's fixed channels have no refinement and the stricter gate, which
// costs about 20 percentage points at -14 dB for a two-frame message). The outer
// edges extend 5 Hz so a signal exactly at 200 or 3000 Hz lies inside a channel.
std::vector<Channel> make_channels() {
    std::vector<Channel> channels;
    const double rx_lo = receiver_rx_frequency_hz - receiver_tolerance_hz;
    const double rx_hi = receiver_rx_frequency_hz + receiver_tolerance_hz;
    const auto rx_picks = static_cast<unsigned>(
        std::max(2L, std::min(8L, std::lround(receiver_tolerance_hz / (14 * fbin)))));
    channels.push_back({std::max(3, bin_floor(rx_lo)), bin_floor(rx_hi), receiver_rx_frequency_hz,
                        receiver_tolerance_hz, true, rx_picks});
    const std::array<std::pair<double, double>, 10> fixed = {{{1200, 1500}, {1500, 1800}, {195, 300},
                                                              {300, 600}, {600, 900}, {900, 1200},
                                                              {1800, 2100}, {2100, 2400}, {2400, 2700},
                                                              {2700, 3005}}};
    for (const auto& [lo, hi] : fixed) {
        const bool extension = !((lo == 1200 && hi == 1500) || (lo == 1500 && hi == 1800));
        channels.push_back({std::max(3, bin_floor(lo)), bin_floor(hi), 0.5 * (lo + hi), 0.5 * (hi - lo),
                            false, 2, extension});
    }
    return channels;
}

// ------------------------------------------------------------- shared tables
struct Tables {
    detail::Fft analytic{analytic_size};
    detail::Fft baseband{baseband_size};
    detail::SplitFft map{map_size};
    std::vector<Cf> sync_conj;                      // conj(q[r]), r < 2496 (D5)
    std::array<std::array<Cf, n_sym>, 4> tone_conj;  // exp(-j 2 pi k r / n)
    std::vector<double> smoother;                   // 385-tap cos^2, unit sum (D12.1)
    double smoother_sum{};                          // sum of the unnormalized taps
    std::vector<Channel> channels = make_channels();
    int union_lo{};
    int union_hi{};

    Tables() {
        sync_conj.resize(sync_length_fd);
        double phase = 0;
        for (int i = 0; i < sync_symbols; ++i) {
            for (int r = 0; r < n_sym; ++r) {
                sync_conj[static_cast<std::size_t>(i * n_sym + r)] =
                    Cf(static_cast<float>(std::cos(phase)), static_cast<float>(-std::sin(phase)));
                phase += two_pi * sync_tones[static_cast<std::size_t>(i)] / n_sym;
            }
        }
        for (int k = 0; k < 4; ++k) {
            for (int r = 0; r < n_sym; ++r) {
                const double a = -two_pi * k * r / n_sym;
                tone_conj[static_cast<std::size_t>(k)][static_cast<std::size_t>(r)] =
                    Cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
            }
        }
        double total = 0;
        for (int j = -n_sym; j <= n_sym; ++j) {
            const double c = std::cos(std::numbers::pi * j / (2.0 * n_sym));
            smoother.push_back(c * c);
            total += c * c;
        }
        smoother_sum = total;
        for (auto& w : smoother) w /= total;
        union_lo = channels.front().b_lo;
        union_hi = channels.front().b_hi;
        for (const auto& c : channels) {
            union_lo = std::min(union_lo, c.b_lo);
            union_hi = std::max(union_hi, c.b_hi);
        }
    }
};

const Tables& tables() {
    static const Tables instance;
    return instance;
}

// -------------------------------------------------------------- frame entries
struct RecordedSignal {
    double frequency_hz{};
    double start_12k{};
    ToneFrame tones{};
};

// ----------------------------------------------------------------- the stream
class Stream {
public:
    Stream(std::span<const float> pcm, ReceiveReport& report)
        : report_(report), t_(tables()),
          assembler_(report.work, max_frame_history, [this](const OpenMessage& m) { complete(m); }) {
        emitted_.assign(static_cast<std::size_t>(lead_12k) + pcm.size() + static_cast<std::size_t>(tail_12k), 0.0F);
        std::copy(pcm.begin(), pcm.end(), emitted_.begin() + lead_12k);
        x_.resize(baseband_size);
        map_.resize(static_cast<std::size_t>(map_steps) * map_bins());
        scratch_.resize(analytic_size);
    }

    void run() {
        const auto total = static_cast<std::int64_t>(emitted_.size());
        for (std::int64_t p = 0; p + window_12k <= total && !failed(); p += step_12k) {
            assembler_.expire(static_cast<double>(p) / fs);
            ++report_.work.windows;
            std::vector<RecordedSignal> recorded;
            analyze(p, nullptr, &recorded);
            std::size_t lookbacks = 0;
            for (const auto& signal : recorded) {
                for (int k = 1; k <= 3 && !failed(); ++k) {
                    const std::int64_t earlier = p - k * step_12k;
                    if (earlier < 0 || lookbacks >= receiver_max_lookback_windows) continue;
                    ++lookbacks;
                    ++report_.work.lookback_windows;
                    analyze(earlier, &signal, nullptr);
                }
            }
        }
        if (failed()) return;
        // End of reception: every open message is cut off (D13.5).
        assembler_.finish();
    }

    [[nodiscard]] bool failed() const { return !failure_.empty(); }
    [[nodiscard]] const std::string& failure() const { return failure_; }

private:
    void fail(std::string reason) {
        if (failure_.empty()) failure_ = std::move(reason);
    }

    [[nodiscard]] std::size_t map_bins() const {
        return static_cast<std::size_t>(t_.union_hi - t_.union_lo + 1);
    }
    float& map_at(int step, int bin) {
        return map_[static_cast<std::size_t>(step) * map_bins() + static_cast<std::size_t>(bin - t_.union_lo)];
    }

    // D3: analytic signal of the window at 6 kHz.
    bool load_window(std::int64_t p) {
        const auto* s = emitted_.data() + p;
        if (std::all_of(s, s + window_12k, [](float v) { return v == 0.0F; })) return false;
        const float scale = 2.0F / static_cast<float>(analytic_size);
        std::fill(scratch_.begin(), scratch_.end(), Cf(0.0F, 0.0F));
        for (std::int64_t i = 0; i < window_12k; ++i) scratch_[static_cast<std::size_t>(i)] = Cf(s[i] * scale, 0.0F);
        t_.analytic.forward(scratch_);
        const auto keep = static_cast<std::size_t>(analytic_passband_hz / fs * analytic_size);
        std::fill(x_.begin(), x_.end(), Cf(0.0F, 0.0F));
        for (std::size_t k = 0; k <= keep; ++k) x_[k] = scratch_[k];
        x_[0] *= 0.5F;
        t_.baseband.inverse(x_);
        std::fill(x_.begin() + window_fd, x_.end(), Cf(0.0F, 0.0F));
        return true;
    }

    // D5: coarse sync map over the union of the channels.
    void build_map() {
        map_stale_ = false;
        std::vector<float> re(map_size);
        std::vector<float> im(map_size);
        std::vector<float> power(map_bins() + 4);
        for (int s = 0; s < map_steps; ++s) {
            const int tau = s * map_step_fd;
            std::fill(re.begin(), re.end(), 0.0F);
            std::fill(im.begin(), im.end(), 0.0F);
            for (int r = 0; r < sync_length_fd; ++r) {
                const Cf v = detail::cmul(x_[static_cast<std::size_t>(tau + r)], t_.sync_conj[static_cast<std::size_t>(r)]);
                re[static_cast<std::size_t>(r)] = v.real();
                im[static_cast<std::size_t>(r)] = v.imag();
            }
            t_.map.forward(re.data(), im.data());
            for (int b = t_.union_lo - 2; b <= t_.union_hi + 2; ++b) {
                const auto u = static_cast<std::size_t>(b);
                power[static_cast<std::size_t>(b - t_.union_lo + 2)] = re[u] * re[u] + im[u] * im[u];
            }
            for (int b = t_.union_lo; b <= t_.union_hi; ++b) {
                const auto i = static_cast<std::size_t>(b - t_.union_lo + 2);
                map_at(s, b) = power[i - 2] + 2.0F * power[i - 1] + 3.0F * power[i] + 2.0F * power[i + 1] + power[i + 2];
            }
        }
    }

    // Mixed (baseband-shifted) signal y[m] = x[m] exp(-j 2 pi f (m + 1) / fd)
    // over [first, first + count); indices outside the buffer read zero.
    void mix(double frequency_hz, int first, int count, std::vector<Cf>& out) const {
        out.assign(static_cast<std::size_t>(std::max(0, count)), Cf(0.0F, 0.0F));
        const double w = -two_pi * frequency_hz / fd;
        const Complex step(std::cos(w), std::sin(w));
        const double a0 = std::remainder(w * (first + 1), two_pi);
        Complex rot(std::cos(a0), std::sin(a0));
        for (int i = 0; i < count; ++i) {
            const int m = first + i;
            if (m >= 0 && m < static_cast<int>(baseband_size)) {
                out[static_cast<std::size_t>(i)] = detail::cmul(
                    x_[static_cast<std::size_t>(m)], Cf(static_cast<float>(rot.real()), static_cast<float>(rot.imag())));
            }
            rot = detail::cmul(rot, step);
            if ((i & 511) == 511) rot /= std::abs(rot);
        }
    }

    // D7: refinement of a channel-Rx candidate. Returns (f, offset in fd samples).
    std::pair<double, int> refine(double f, double x_s) {
        const int t_a = std::max(0, static_cast<int>(std::lround((x_s - 0.004) * fd)));
        const int t_b = std::min(window_fd - sync_symbols * n_sym, static_cast<int>(std::lround((x_s + 0.004) * fd)));
        double best = 0;
        double best_f = 0;
        int best_t = 0;
        std::array<Complex, sync_symbols> best_z{};
        std::vector<Cf> y;
        for (int k = -5; k <= 5; ++k) {
            const double fh = f - 0.5 * k;
            if (t_a > t_b) break;
            mix(fh, t_a, t_b - t_a + sync_length_fd, y);
            for (int t = t_a; t <= t_b; t += 4) {
                double metric = 0;
                std::array<Complex, sync_symbols> z{};
                for (int i = 0; i < sync_symbols; ++i) {
                    Complex sum{};
                    const int base = t - t_a + i * n_sym;
                    for (int r = 0; r < n_sym; ++r) {
                        sum += detail::cmul(Complex(y[static_cast<std::size_t>(base + r)]),
                               Complex(t_.sync_conj[static_cast<std::size_t>(i * n_sym + r)]));
                    }
                    z[static_cast<std::size_t>(i)] = sum;
                    metric += std::norm(sum);
                }
                if (metric > best) {
                    best = metric;
                    best_f = fh;
                    best_t = t;
                    best_z = z;
                }
            }
        }
        if (!(best > 0)) return {0.0, 0};
        // Linear-phase correction.
        std::array<double, sync_symbols> u{};
        u[0] = std::arg(best_z[0]);
        for (int i = 1; i < sync_symbols; ++i) {
            double d = std::arg(best_z[static_cast<std::size_t>(i)]) - std::arg(best_z[static_cast<std::size_t>(i - 1)]);
            while (d > std::numbers::pi) d -= two_pi;
            while (d < -std::numbers::pi) d += two_pi;
            u[static_cast<std::size_t>(i)] = u[static_cast<std::size_t>(i - 1)] + d;
        }
        double mean = 0;
        for (const double v : u) mean += v;
        mean /= sync_symbols;
        double num = 0;
        double den = 0;
        for (int i = 0; i < sync_symbols; ++i) {
            num += (i - 6) * (u[static_cast<std::size_t>(i)] - mean);
            den += (i - 6) * (i - 6);
        }
        const double beta = num / den;
        const double a = mean - 6 * beta;
        double residual = 0;
        for (int i = 0; i < sync_symbols; ++i) {
            const double e = u[static_cast<std::size_t>(i)] - a - beta * i;
            residual += e * e;
        }
        residual = std::sqrt(residual / sync_symbols);
        const double df = beta / (two_pi * 0.032);
        if (residual < 1.0 && std::abs(df) <= 0.5) best_f += df;
        return {best_f, best_t};
    }

    // D8: sync-quality gate at (f, t).
    bool gate(double f, int t, const Channel& channel) {
        std::vector<Cf> y;
        mix(f, t, sync_length_fd, y);
        int hits = 0;
        double on = 0;
        double total = 0;
        for (int i = 0; i < sync_symbols; ++i) {
            const int ti = t + i * n_sym;
            if (ti < 0 || ti + n_sym > window_fd) continue;
            std::array<double, 4> w{};
            for (int k = 0; k < 4; ++k) {
                Complex sum{};
                for (int r = 0; r < n_sym; ++r) {
                    sum += detail::cmul(Complex(y[static_cast<std::size_t>(i * n_sym + r)]),
                           Complex(t_.tone_conj[static_cast<std::size_t>(k)][static_cast<std::size_t>(r)]));
                }
                w[static_cast<std::size_t>(k)] = std::norm(sum);
            }
            const auto expected = sync_tones[static_cast<std::size_t>(i)];
            const auto strongest = static_cast<std::size_t>(std::max_element(w.begin(), w.end()) - w.begin());
            if (strongest == expected) ++hits;
            on += w[expected];
            total += w[0] + w[1] + w[2] + w[3];
        }
        const double off = (total - on) / 3.0;
        const double ratio = off > 0 ? on / off : 0.0;
        const double rho = off > 0 ? (ratio > 1.259e-10 ? 10.0 * std::log10(ratio) : -99.0) : -99.9;
        if (channel.rx || channel.extension) return hits >= rx_min_hits && rho >= rx_min_rho_db;
        return hits >= fixed_min_hits && rho >= fixed_min_rho_db;
    }

    // Mean energy at the decoded tones over the mean energy at the other three
    // tones, over the data symbols (the plausibility test above).
    static double decoded_tone_ratio(const detail::SoftObservations& observations, const ToneFrame& tones) {
        double decoded = 0;
        double other = 0;
        for (unsigned k = 0; k < detail::fec_data_symbols; ++k) {
            const unsigned tone = tones[sync_symbols + k];
            for (unsigned q = 0; q < 4; ++q) {
                const double e = std::norm(std::complex<double>(observations.full[k][q]));
                (q == tone ? decoded : other) += e;
            }
        }
        return other > 0 ? decoded / (other / 3.0) : 1e9;
    }

    // D9: payload observations at (f, t).
    detail::SoftObservations observe(double f, int t) {
        detail::SoftObservations o;
        const int payload = t + sync_symbols * n_sym;
        std::vector<Cf> y;
        mix(f, payload, data_symbols * n_sym, y);
        for (int m = 0; m < data_symbols; ++m) {
            const int start = payload + m * n_sym;
            if (start < 0 || start + n_sym > window_fd) continue;  // zero observation
            for (int k = 0; k < 4; ++k) {
                Complex a{};
                Complex d{};
                for (int r = 0; r < n_sym; ++r) {
                    const Complex v = detail::cmul(Complex(y[static_cast<std::size_t>(m * n_sym + r)]),
                                      Complex(t_.tone_conj[static_cast<std::size_t>(k)][static_cast<std::size_t>(r)]));
                    (r < n_sym / 2 ? a : d) += v;
                }
                const Complex c = a + d;
                o.full[static_cast<std::size_t>(m)][static_cast<std::size_t>(k)] =
                    std::complex<float>(static_cast<float>(c.real()), static_cast<float>(c.imag()));
                o.split[static_cast<std::size_t>(m)][static_cast<std::size_t>(k)] =
                    static_cast<float>(std::sqrt(std::norm(a) + std::norm(d)));
            }
        }
        return o;
    }

    // Reporting estimate (not part of the WSJT-X decoder): the decoded frame's
    // full known trajectory, coherent over four-symbol segments, refines the
    // start (fractional fd samples) and frequency on the current residual. It
    // only sharpens the reported values, the assembly times and the
    // subtraction placement; the decode itself used the search estimate.
    // Metric of the known frame at integer offsets [t_first, t_last] for one
    // frequency: sum over four-symbol segments of |sum z[t+i] conj(ref[i])|^2.
    void frame_metrics(const std::vector<Cf>& reference, double f, int t_first, int t_last,
                       std::vector<double>& out, int t_step = 1) const {
        constexpr int segment = 4 * n_sym;
        const int span = t_last - t_first + frame_fd;
        std::vector<Complex> z(static_cast<std::size_t>(span));
        const double w = -two_pi * f / fd;
        const Complex step(std::cos(w), std::sin(w));
        Complex rot(std::cos(w * t_first), std::sin(w * t_first));
        for (int i = 0; i < span; ++i) {
            const int m = t_first + i;
            if (m >= 0 && m < static_cast<int>(baseband_size)) {
                z[static_cast<std::size_t>(i)] = detail::cmul(Complex(x_[static_cast<std::size_t>(m)]), rot);
            }
            rot = detail::cmul(rot, step);
            if ((i & 1023) == 1023) rot /= std::abs(rot);
        }
        out.assign(static_cast<std::size_t>(t_last - t_first + 1), 0.0);
        for (int t = t_first; t <= t_last; t += t_step) {
            double total = 0;
            for (int begin = 0; begin < frame_fd; begin += segment) {
                const int end = std::min(frame_fd, begin + segment);
                Complex sum{};
                for (int i = begin; i < end; ++i) {
                    sum += detail::cmulconj(z[static_cast<std::size_t>(t - t_first + i)],
                                            Complex(reference[static_cast<std::size_t>(i)]));
                }
                total += std::norm(sum);
            }
            out[static_cast<std::size_t>(t - t_first)] = total;
        }
    }

    static double parabola_offset(double below, double centre, double above) {
        const double den = below - 2.0 * centre + above;
        if (!(den < 0)) return 0.0;
        const double offset = 0.5 * (below - above) / den;
        return std::abs(offset) <= 1.0 ? offset : 0.0;
    }

    // Returns (frequency, fractional start offset in fd samples).
    std::pair<double, double> report_estimate(const ToneFrame& tones, double f0, int t0) {
        const auto reference = detail::gfsk_baseband(tones, n_sym, 0.0, 0, frame_fd, 0.0);
        std::vector<double> metrics;
        // Coarse joint grid: +-24 samples (4 ms) in steps of 3, +-5 Hz in 0.5 Hz.
        double best = -1;
        double best_f = f0;
        int best_t = std::max(0, t0);
        const int lo = std::max(0, t0 - 24);
        const int hi = std::max(lo, t0 + 24);
        for (int k = -10; k <= 10; ++k) {
            const double f = f0 + 0.5 * k;
            frame_metrics(reference, f, lo, hi, metrics, 3);
            for (int t = lo; t <= hi; t += 3) {
                const double m = metrics[static_cast<std::size_t>(t - lo)];
                if (m > best) {
                    best = m;
                    best_f = f;
                    best_t = t;
                }
            }
        }
        // Fine grid: +-2 samples, +-0.5 Hz in 0.1 Hz steps.
        const int flo = std::max(0, best_t - 3);
        const int fhi = best_t + 3;
        const double coarse_f = best_f;
        std::vector<std::vector<double>> rows;
        int best_k = 0;
        for (int k = -6; k <= 6; ++k) {
            frame_metrics(reference, coarse_f + 0.1 * k, flo, fhi, metrics);
            rows.push_back(metrics);
            for (int t = std::max(flo, best_t - 2); t <= std::min(fhi, best_t + 2); ++t) {
                const double m = metrics[static_cast<std::size_t>(t - flo)];
                if (m > best) {
                    best = m;
                    best_f = coarse_f + 0.1 * k;
                    best_t = t;
                }
            }
        }
        // Re-locate the best cell in the fine table and interpolate.
        best_k = static_cast<int>(std::lround((best_f - coarse_f) / 0.1));
        const auto row = [&](int k) -> const std::vector<double>& { return rows[static_cast<std::size_t>(k + 6)]; };
        const auto ti = static_cast<std::size_t>(best_t - flo);
        double f_out = best_f;
        if (best_k > -6 && best_k < 6) {
            f_out += 0.1 * parabola_offset(row(best_k - 1)[ti], row(best_k)[ti], row(best_k + 1)[ti]);
        }
        double t_out = best_t;
        if (best_t > flo && best_t < fhi) {
            const auto& r = row(best_k);
            t_out += parabola_offset(r[ti - 1], r[ti], r[ti + 1]);
        }
        return {f_out, t_out};
    }

    // D12.1: subtraction of a known frame placed at fractional fd offset t.
    void subtract(const ToneFrame& tones, double f, double t) {
        const int first = static_cast<int>(std::floor(t));
        const int count = frame_fd + 2;
        auto psi = detail::gfsk_baseband(tones, n_sym, t - first, 0, static_cast<std::size_t>(count), n_sym / 8.0);
        const double w = two_pi * f / fd;
        for (int i = 0; i < count; ++i) {
            const double a = std::remainder(w * (first + i), two_pi);
            psi[static_cast<std::size_t>(i)] =
                detail::cmul(psi[static_cast<std::size_t>(i)], Cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))));
        }
        const int lo = std::max(0, -first);
        const int hi = std::min(count, window_fd - first);
        if (lo >= hi) return;
        map_stale_ = true;
        std::vector<Complex> gain(static_cast<std::size_t>(count), Complex{});
        for (int i = lo; i < hi; ++i) {
            gain[static_cast<std::size_t>(i)] = detail::cmulconj(Complex(x_[static_cast<std::size_t>(first + i)]),
                                                                 Complex(psi[static_cast<std::size_t>(i)]));
        }
        // Smoothed gain at i: sum_j w_j g[i + 1 - j], j = -n..n, zero outside.
        // With w_j = (1 + cos(pi j / n)) / (2 W) (the cos^2 kernel, W its sum)
        // this is three windowed sums of g, e^{-j pi k / n} g and e^{+j pi k / n} g,
        // evaluated exactly with prefix sums (linear, not 385 taps per sample).
        const auto unit = [](int k) {  // e^{j pi k / n}, exact period 2n
            const int r = ((k % (2 * n_sym)) + 2 * n_sym) % (2 * n_sym);
            const double a = std::numbers::pi * r / n_sym;
            return Complex(std::cos(a), std::sin(a));
        };
        const auto span = static_cast<std::size_t>(count + 1);
        std::vector<Complex> p0(span), pm(span), pp(span);  // prefix sums over k < index
        for (int k = 0; k < count; ++k) {
            const Complex g = (k >= lo && k < hi) ? gain[static_cast<std::size_t>(k)] : Complex{};
            const Complex e = unit(k);
            const auto u = static_cast<std::size_t>(k);
            p0[u + 1] = p0[u] + g;
            pm[u + 1] = pm[u] + detail::cmulconj(g, e);
            pp[u + 1] = pp[u] + detail::cmul(g, e);
        }
        const double scale = 1.0 / (2.0 * t_.smoother_sum);
        std::vector<Complex> smooth(static_cast<std::size_t>(count), Complex{});
        for (int i = lo; i < hi; ++i) {
            const int a = std::max(0, i + 1 - n_sym);
            const int b = std::min(count, i + 2 + n_sym);  // exclusive
            if (a >= b) continue;
            const auto ua = static_cast<std::size_t>(a);
            const auto ub = static_cast<std::size_t>(b);
            const Complex phase = unit(i + 1);
            const Complex sum = (p0[ub] - p0[ua]) + 0.5 * detail::cmul(phase, pm[ub] - pm[ua]) +
                                0.5 * detail::cmulconj(pp[ub] - pp[ua], phase);
            smooth[static_cast<std::size_t>(i)] = scale * sum;
        }
        for (int i = lo; i < hi; ++i) {
            const Complex r = detail::cmul(smooth[static_cast<std::size_t>(i)], Complex(psi[static_cast<std::size_t>(i)]));
            x_[static_cast<std::size_t>(first + i)] -= Cf(static_cast<float>(r.real()), static_cast<float>(r.imag()));
        }
    }

    struct WindowState {
        std::int64_t p{};
        std::size_t records{};
        detail::WindowDuplicates duplicates;  // D13.1 a-b, D6 vicinity records
        std::vector<RecordedSignal>* recorded{};
        bool subtracted{};
        bool lookback{};
        int pass{1};
    };

    enum class Attempt { nothing, decoded };

    // Decode, validate, test duplicates, subtract and assemble one candidate.
    Attempt attempt(WindowState& w, const Channel& channel, double f, int t) {
        const auto observations = observe(f, t);
        const auto fec = detail::decode_tiers(observations);
        report_.work.fec_hypotheses += fec.crc_hypotheses;
        if (!fec.payload) return Attempt::nothing;
        const auto atom = decode_frame_payload(*fec.payload);  // D11
        if (!atom) return Attempt::nothing;
        const auto encoded = encode_payload(*fec.payload);
        if (!encoded) return Attempt::nothing;
        const double required_ratio = channel.extension ? min_decoded_tone_ratio_extension : min_decoded_tone_ratio;
        if (decoded_tone_ratio(observations, encoded.value.tones) < required_ratio) return Attempt::nothing;
        ++report_.work.frames_decoded;
        if (report_.work.frames_decoded > receiver_max_decoded_frames) {
            fail("receiver work budget exceeded: more than " + std::to_string(receiver_max_decoded_frames) + " decoded frames in one reception");
            return Attempt::nothing;
        }

        FrameEntry entry;
        entry.payload = *fec.payload;
        entry.eom = atom.value.end_of_message;
        if (atom.value.atom == "TEXT5") {
            entry.text = atom.value.text5_raw;
            std::replace(entry.text.begin(), entry.text.end(), ' ', stand_in);
            entry.space_after = false;
        } else {
            entry.text = atom.value.rendering;
            entry.space_after = true;
        }
        // A frame already assembled (same payload, within the stamp radius of
        // the search estimate; typically a look-back re-decode) reuses its stored
        // reporting estimate instead of recomputing it. It is then a stamp
        // duplicate (D13.1 c) exactly as a recomputed estimate would make it.
        const double search_seconds = (static_cast<double>(w.p) + 2.0 * t) / fs;
        const FrameEntry* known = nullptr;
        for (const auto& old : assembler_.history()) {
            if (old.payload == entry.payload && std::abs(old.frequency_hz - f) < stamp_df_hz &&
                std::abs(old.seconds() - search_seconds) < stamp_dt_s) {
                known = &old;
                break;
            }
        }
        const auto [rf, rt] = known != nullptr
            ? std::pair<double, double>{known->frequency_hz, (known->start_12k - static_cast<double>(w.p)) / 2.0}
            : report_estimate(encoded.value.tones, f, t);
        entry.frequency_hz = rf;
        entry.start_12k = static_cast<double>(w.p) + 2.0 * rt;
        const double seconds = entry.seconds();
        // D13.1 a and b.
        const auto verdict = w.duplicates.test(entry.text, rf, seconds, !channel.rx);
        if (verdict != detail::WindowDuplicates::Verdict::fresh) {
            ++report_.work.duplicate_frames;
            if (verdict == detail::WindowDuplicates::Verdict::rx_repeat) w.duplicates.add_decoded(entry.text, seconds);
            return Attempt::decoded;
        }
        w.duplicates.add_decoded(entry.text, seconds);
        // D12.1: subtract and record.
        subtract(encoded.value.tones, rf, rt);
        ++report_.work.subtractions;
        w.subtracted = true;
        if (w.recorded != nullptr && w.recorded->size() < max_recorded_subtractions) {
            w.recorded->push_back({rf, entry.start_12k, encoded.value.tones});
        }
        if (channel.rx) w.duplicates.add_rx(rf, seconds);
        const auto merged = assembler_.merge(entry);
        if (merged == MergeResult::history_full) {
            fail("receiver frame history bound exceeded");
        } else if (merged != MergeResult::stamp_duplicate && merged != MergeResult::alias_duplicate) {
            if (w.lookback) ++report_.work.lookback_frames;
            if (w.pass == 2) ++report_.work.second_pass_frames;
        }
        return Attempt::decoded;
    }

    // D13.4: expected-frame retry after a pass that decoded nothing.
    void retry(WindowState& w, const Channel& channel) {
        const double window_s = static_cast<double>(w.p) / fs;
        for (const auto& message : assembler_.open()) {
            if (message.f_last < channel.center_hz - channel.half_width_hz ||
                message.f_last > channel.center_hz + channel.half_width_hz) {
                continue;
            }
            if (std::abs((window_s - message.t_last) - frame_period_s) > retry_timing_s) continue;
            const double x = message.t_last + frame_period_s - window_s;
            if (x < 0) continue;
            if (w.records >= receiver_max_window_candidates) return;
            ++w.records;
            ++report_.work.candidates;
            ++report_.work.retries;
            const double f = message.f_last;  // copy: merge() may reorder the open list
            attempt(w, channel, f, static_cast<int>(std::lround(x * fd)));
            return;
        }
    }

    // One pass of one channel (D6 picks, D7 refinement, D8 gate).
    void channel_pass(WindowState& w, const Channel& channel, std::vector<std::uint8_t>* exclusion) {
        bool decoded = false;
        for (unsigned pick = 0; pick < channel.picks; ++pick) {
            if (failed() || w.records >= receiver_max_window_candidates) return;
            float best = -1.0F;
            int best_b = -1;
            int best_s = -1;
            for (int s = 0; s < map_steps; ++s) {
                if (w.p + 2 * static_cast<std::int64_t>(s) * map_step_fd - lead_12k < -search_reach_12k) continue;
                for (int b = channel.b_lo; b <= channel.b_hi; ++b) {
                    if (exclusion != nullptr &&
                        (*exclusion)[static_cast<std::size_t>(s) * map_bins() + static_cast<std::size_t>(b - t_.union_lo)] != 0) {
                        continue;
                    }
                    const float g = map_at(s, b);
                    if (g > best) {
                        best = g;
                        best_b = b;
                        best_s = s;
                    }
                }
            }
            if (best_b < 0) return;  // fully excluded (D16 U3): stop the channel
            for (int s = std::max(0, best_s - exclude_steps_below); s <= std::min(map_steps - 1, best_s + exclude_steps_above); ++s) {
                for (int b = std::max(channel.b_lo, best_b - exclude_bins_below);
                     b <= std::min(channel.b_hi, best_b + exclude_bins_above); ++b) {
                    if (exclusion != nullptr) {
                        (*exclusion)[static_cast<std::size_t>(s) * map_bins() + static_cast<std::size_t>(b - t_.union_lo)] = 1;
                    } else {
                        map_at(s, b) = 0.0F;
                    }
                }
            }
            ++w.records;
            ++report_.work.candidates;
            double f = best_b * fbin;
            int t = best_s * map_step_fd;
            if (channel.rx || channel.extension) {
                const auto refined = refine(f, t / fd);
                f = refined.first;
                t = refined.second;
            }
            if (!gate(f, t, channel)) continue;
            ++report_.work.gated_candidates;
            if (attempt(w, channel, f, t) == Attempt::decoded) decoded = true;
        }
        if (!decoded && !failed()) retry(w, channel);
    }

    void zero_rx_vicinity(const WindowState& w) {
        for (const auto& rx : w.duplicates.rx()) {
            const auto centre = static_cast<int>(std::lround(rx.frequency_hz / fbin));
            for (int b = std::max(t_.union_lo, centre - vicinity_bins); b <= std::min(t_.union_hi, centre + vicinity_bins); ++b) {
                for (int s = 0; s < map_steps; ++s) map_at(s, b) = 0.0F;
            }
        }
    }

    // D1: one window evaluation (forward or look-back).
    void analyze(std::int64_t p, const RecordedSignal* lookback, std::vector<RecordedSignal>* recorded) {
        if (failed()) return;
        if (++evaluations_ > receiver_max_window_evaluations) {
            fail("receiver work budget exceeded: more than " + std::to_string(receiver_max_window_evaluations) + " window evaluations in one reception");
            return;
        }
        if (!load_window(p)) {
            ++report_.work.silent_windows;
            return;
        }
        if (lookback != nullptr) {
            subtract(lookback->tones, lookback->frequency_hz, (lookback->start_12k - static_cast<double>(p)) / 2.0);
        }
        WindowState w;
        w.p = p;
        w.recorded = recorded;
        w.lookback = lookback != nullptr;
        const auto& rx = t_.channels.front();
        // Rx-channel round.
        build_map();
        std::vector<std::uint8_t> exclusion(static_cast<std::size_t>(map_steps) * map_bins(), 0);
        for (int pass = 1; pass <= 2; ++pass) {
            if (pass == 2) {
                if (!w.subtracted) break;
                build_map();
            }
            w.subtracted = false;
            w.pass = pass;
            std::fill(exclusion.begin(), exclusion.end(), std::uint8_t{0});
            channel_pass(w, rx, &exclusion);
        }
        // Fixed-channel round. Pass 1 rebuilds the map only when a subtraction
        // happened since it was built; pass 2 always uses a rebuilt map.
        for (int pass = 1; pass <= 2; ++pass) {
            if (pass == 2 || map_stale_) build_map();
            w.subtracted = false;
            w.pass = pass;
            zero_rx_vicinity(w);
            for (std::size_t c = 1; c < t_.channels.size(); ++c) channel_pass(w, t_.channels[c], nullptr);
            if (!w.subtracted) break;
        }
    }

    void complete(const OpenMessage& m) {
        const std::string text = detail::publication_text(m.text);
        if (text.empty()) return;
        const long frequency = std::lround(m.f_last);
        if (frequency < receiver_search_low_hz || frequency > receiver_search_high_hz) {
            ++report_.work.rejected_messages;
            return;
        }
        report_.results.push_back({text, static_cast<int>(frequency),
                                   static_cast<std::int64_t>(std::llround(m.first.start_12k)) - lead_12k, m.frames});
        if (report_.results.size() > receiver_max_results) {
            fail("completed message count exceeds the " + std::to_string(receiver_max_results) + "-result bound");
        }
    }

    ReceiveReport& report_;
    const Tables& t_;
    std::vector<float> emitted_;
    std::vector<Cf> x_;
    std::vector<Cf> scratch_;
    std::vector<float> map_;
    bool map_stale_{true};
    detail::Assembler assembler_;
    std::size_t evaluations_{};
    std::string failure_;
};

}  // namespace

std::string_view receive_outcome_name(ReceiveOutcome outcome) {
    switch (outcome) {
    case ReceiveOutcome::accepted_complete: return "accepted-complete";
    case ReceiveOutcome::multiple_result: return "multiple-result";
    case ReceiveOutcome::no_result: break;
    }
    return "no-result";
}

Result<ReceiveReport> receive_messages(std::span<const float> pcm_12k) {
    if (pcm_12k.empty()) return {.error = "receiver input is empty"};
    if (pcm_12k.size() > receiver_max_input_samples) {
        return {.error = "receiver input exceeds the 1,440,000-sample bound"};
    }
    if (!std::all_of(pcm_12k.begin(), pcm_12k.end(), [](float v) { return std::isfinite(v); })) {
        return {.error = "receiver input contains a non-finite sample"};
    }
    ReceiveReport report;
    report.work.input_samples = pcm_12k.size();
    Stream stream(pcm_12k, report);
    stream.run();
    if (stream.failed()) return {.error = stream.failure()};
    std::sort(report.results.begin(), report.results.end(),
              [](const ReceivedMessage& a, const ReceivedMessage& b) {
                  return std::tie(a.start_sample_12000, a.frequency_hz, a.text) <
                         std::tie(b.start_sample_12000, b.frequency_hz, b.text);
              });
    if (report.results.size() == 1) report.outcome = ReceiveOutcome::accepted_complete;
    else if (report.results.size() > 1) report.outcome = ReceiveOutcome::multiple_result;
    return {.value = std::move(report)};
}

}  // namespace jtty
