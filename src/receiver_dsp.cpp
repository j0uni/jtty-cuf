#include "receiver_dsp.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <numbers>

namespace jtty::detail {

Fft::Fft(std::size_t size) : size_(size), reversed_(size), twiddle_(size > 1 ? size - 1 : 0) {
    const unsigned bits = static_cast<unsigned>(std::countr_zero(size));
    for (std::size_t i = 0; i < size; ++i) {
        std::size_t r = 0;
        for (unsigned b = 0; b < bits; ++b) r |= ((i >> b) & 1U) << (bits - 1U - b);
        reversed_[i] = r;
    }
    // Stage tables stored contiguously: the stage with butterfly span `half`
    // uses twiddle_[half - 1 + k] = exp(-j pi k / half), k < half.
    for (std::size_t half = 1; half < size; half *= 2) {
        for (std::size_t k = 0; k < half; ++k) {
            const double angle = -std::numbers::pi * static_cast<double>(k) / static_cast<double>(half);
            twiddle_[half - 1 + k] = Cf(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
        }
    }
}

void Fft::transform(std::span<Cf> data, bool inverse) const {
    // The inverse transform is conj(forward(conj(x))).
    if (inverse) for (auto& v : data) v = std::conj(v);
    for (std::size_t i = 0; i < size_; ++i) {
        const std::size_t r = reversed_[i];
        if (i < r) std::swap(data[i], data[r]);
    }
    Cf* d = data.data();
    for (std::size_t half = 1; half < size_; half *= 2) {
        const Cf* w = twiddle_.data() + (half - 1);
        for (std::size_t start = 0; start < size_; start += 2 * half) {
            Cf* lo = d + start;
            Cf* hi = lo + half;
            for (std::size_t k = 0; k < half; ++k) {
                const Cf b = cmul(hi[k], w[k]);
                const Cf a = lo[k];
                lo[k] = a + b;
                hi[k] = a - b;
            }
        }
    }
    if (inverse) for (auto& v : data) v = std::conj(v);
}

SplitFft::SplitFft(std::size_t size) : size_(size), wr_(size), wi_(size) {
    std::size_t offset = 0;
    for (std::size_t length = size; length >= 2; length /= 2) {
        for (std::size_t p = 0; p < length / 2; ++p) {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>(p) / static_cast<double>(length);
            wr_[offset + p] = static_cast<float>(std::cos(angle));
            wi_[offset + p] = static_cast<float>(std::sin(angle));
        }
        offset += length / 2;
    }
}

void SplitFft::forward(float* re, float* im) const {
    float* ar = re;
    float* ai = im;
    thread_local std::vector<float> scratch_re;
    thread_local std::vector<float> scratch_im;
    if (scratch_re.size() < size_) {
        scratch_re.resize(size_);
        scratch_im.resize(size_);
    }
    float* cr = scratch_re.data();
    float* ci = scratch_im.data();
    std::size_t offset = 0;
    for (std::size_t n = size_, s = 1; n >= 2; n /= 2, s *= 2) {
        const std::size_t m = n / 2;
        const float* tr = wr_.data() + offset;
        const float* ti = wi_.data() + offset;
        offset += m;
        for (std::size_t p = 0; p < m; ++p) {
            const float w0 = tr[p];
            const float w1 = ti[p];
            const float* a0r = ar + s * p;
            const float* a0i = ai + s * p;
            const float* a1r = ar + s * (p + m);
            const float* a1i = ai + s * (p + m);
            float* y0r = cr + s * 2 * p;
            float* y0i = ci + s * 2 * p;
            float* y1r = y0r + s;
            float* y1i = y0i + s;
            for (std::size_t q = 0; q < s; ++q) {
                const float pr = a0r[q];
                const float pi = a0i[q];
                const float qr = a1r[q];
                const float qi = a1i[q];
                y0r[q] = pr + qr;
                y0i[q] = pi + qi;
                const float dr = pr - qr;
                const float di = pi - qi;
                y1r[q] = dr * w0 - di * w1;
                y1i[q] = dr * w1 + di * w0;
            }
        }
        std::swap(ar, cr);
        std::swap(ai, ci);
    }
    if (ar != re) {
        std::copy(ar, ar + size_, re);
        std::copy(ai, ai + size_, im);
    }
}

namespace {

double gaussian_pulse(double t) {
    // Public frequency pulse (protocol.md "Waveform"), BT = 2.
    const double c = std::numbers::pi * std::sqrt(2.0 / std::log(2.0));
    constexpr double bt = 2.0;
    return 0.5 * (std::erf(c * bt * (t + 0.5)) - std::erf(c * bt * (t - 0.5)));
}

// gaussian_pulse on [-1.5, 1.5] (the three-symbol support used below), tabulated
// with linear interpolation; the interpolation error is below 1e-6.
class PulseTable {
public:
    PulseTable() {
        for (std::size_t i = 0; i < values_.size(); ++i) {
            values_[i] = gaussian_pulse(-1.5 + 3.0 * static_cast<double>(i) / (values_.size() - 1));
        }
    }
    [[nodiscard]] double operator()(double t) const {
        const double x = (t + 1.5) / 3.0 * static_cast<double>(values_.size() - 1);
        if (!(x > 0.0)) return values_.front();
        const auto i = static_cast<std::size_t>(x);
        if (i + 1 >= values_.size()) return values_.back();
        const double f = x - static_cast<double>(i);
        return values_[i] + f * (values_[i + 1] - values_[i]);
    }

private:
    std::array<double, 8193> values_{};
};

const PulseTable& pulse_table() {
    static const PulseTable table;
    return table;
}

// Instantaneous frequency in tone units at frame time u (symbols).
double shaped_tone(std::span<const std::uint8_t> tones, double u) {
    const auto symbol = static_cast<std::int64_t>(std::floor(u));
    const double local = u - static_cast<double>(symbol) - 0.5;
    const auto count = static_cast<std::int64_t>(tones.size());
    const auto tone_at = [&](std::int64_t index) -> double {
        if (index < 0) return tones.front();
        if (index >= count) return tones.back();
        return tones[static_cast<std::size_t>(index)];
    };
    const auto& pulse = pulse_table();
    return tone_at(symbol - 1) * pulse(local + 1.0) + tone_at(symbol) * pulse(local) +
           tone_at(symbol + 1) * pulse(local - 1.0);
}

}  // namespace

std::vector<Cf> gfsk_baseband(std::span<const std::uint8_t> tones, double samples_per_symbol,
                              double delay, std::ptrdiff_t first_sample, std::size_t count,
                              double ramp) {
    std::vector<Cf> out(count, Cf(0.0F, 0.0F));
    if (tones.empty()) return out;
    const double length = static_cast<double>(tones.size()) * samples_per_symbol;  // frame samples
    // Frame-relative sample position of out[i] is p_i = first_sample + i - delay.
    // The phase is the midpoint-rule integral of 2 pi f(t) from the frame start.
    double phase = 0.0;
    double previous = 0.0;  // frame-relative position of the last integrated point
    for (std::size_t i = 0; i < count; ++i) {
        const double p = static_cast<double>(first_sample) + static_cast<double>(i) - delay;
        if (p < 0.0 || p >= length) continue;
        const double step = p - previous;
        const double middle = (previous + p) * 0.5;
        phase += 2.0 * std::numbers::pi * shaped_tone(tones, middle / samples_per_symbol) * step /
                 samples_per_symbol;
        previous = p;
        double envelope = 1.0;
        if (ramp > 0.0) {
            if (p < ramp) envelope = 0.5 * (1.0 - std::cos(std::numbers::pi * p / ramp));
            const double tail = length - p;
            if (tail < ramp) envelope = 0.5 * (1.0 - std::cos(std::numbers::pi * tail / ramp));
        }
        out[i] = Cf(static_cast<float>(envelope * std::cos(phase)), static_cast<float>(envelope * std::sin(phase)));
    }
    return out;
}

}  // namespace jtty::detail
