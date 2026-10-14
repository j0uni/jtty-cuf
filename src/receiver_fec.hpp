#pragma once

// Internal tiered list decoder of the candidate receiver. Not installed.

#include "jtty/protocol.hpp"

#include <array>
#include <complex>
#include <optional>

namespace jtty::detail {

inline constexpr unsigned fec_data_symbols = 46;

// Per data symbol and tone: the full-symbol complex observation C and the
// split-symbol magnitude H (design notes, section 9).
struct SoftObservations {
    std::array<std::array<std::complex<float>, 4>, fec_data_symbols> full{};
    std::array<std::array<float, 4>, fec_data_symbols> split{};
};

struct FecOutcome {
    std::optional<PayloadBits> payload;
    unsigned tier{};            // 1..4 for an accepted payload, 0 otherwise
    unsigned crc_hypotheses{};  // CRC checks performed over all tiers
};

// Tiers of the decoder (design notes, section 10): coherent blocks of 1, 2 and 4
// symbols on C, then single-symbol energy on H. Each tier runs a two-lap
// circular list Viterbi search (four paths per state) and tests at most four
// ranked words; the first CRC-valid word decides the tier.
FecOutcome decode_tiers(const SoftObservations& observations);

}  // namespace jtty::detail
