#pragma once

// Internal numeric building blocks of the candidate receiver. Not installed.

#include "jtty/protocol.hpp"

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace jtty::detail {

using Cf = std::complex<float>;

// Plain complex products. The standard operator* carries the C Annex G
// infinity/NaN recovery, which is an out-of-line call in hot loops; every
// operand here is finite.
template <typename T>
inline std::complex<T> cmul(std::complex<T> a, std::complex<T> b) {
    return {a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real()};
}
template <typename T>
inline std::complex<T> cmulconj(std::complex<T> a, std::complex<T> b) {  // a * conj(b)
    return {a.real() * b.real() + a.imag() * b.imag(), a.imag() * b.real() - a.real() * b.imag()};
}

// Iterative radix-2 complex transform of one fixed power-of-two length.
// forward(): X[b] = sum x[r] exp(-j 2 pi b r / N); inverse(): the same with
// exp(+j ...), unnormalized.
class Fft {
public:
    explicit Fft(std::size_t size);
    [[nodiscard]] std::size_t size() const { return size_; }
    void forward(std::span<Cf> data) const { transform(data, false); }
    void inverse(std::span<Cf> data) const { transform(data, true); }

private:
    void transform(std::span<Cf> data, bool inverse) const;
    std::size_t size_;
    std::vector<std::size_t> reversed_;
    std::vector<Cf> twiddle_;  // per-stage tables, see the constructor
};

// Split-format (separate real and imaginary arrays) Stockham radix-2 forward
// transform, X[b] = sum x[r] exp(-j 2 pi b r / N); the same result as Fft but
// with contiguous inner loops. Used for the sync map. The object is immutable
// after construction; the ping-pong buffers are per-thread scratch, so one
// instance may be shared by concurrent receptions.
class SplitFft {
public:
    explicit SplitFft(std::size_t size);
    [[nodiscard]] std::size_t size() const { return size_; }
    // re and im hold size() values each and receive the transform in place.
    void forward(float* re, float* im) const;

private:
    std::size_t size_;
    std::vector<float> wr_;  // per-stage twiddles, stage of length L: exp(-j 2 pi p / L), p < L / 2
    std::vector<float> wi_;
};

// Continuous-phase Gaussian-FSK (BT = 2, h = 1) baseband trajectory of a tone
// sequence at `rate_hz`, unit magnitude, tone 0 at 0 Hz, phase 0 at the frame
// start. Sample i lies at frame time (i + first_sample - delay) / rate_hz,
// where `delay` (in samples, may be fractional) is the frame start. Samples
// outside the frame are zero; the first and last `ramp` samples of the frame
// carry raised-cosine edges (ramp 0: rectangular frame edges).
std::vector<Cf> gfsk_baseband(std::span<const std::uint8_t> tones, double samples_per_symbol,
                              double delay, std::ptrdiff_t first_sample, std::size_t count,
                              double ramp);

}  // namespace jtty::detail
