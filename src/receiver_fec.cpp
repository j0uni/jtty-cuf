#include "receiver_fec.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <tuple>
#include <vector>

namespace jtty::detail {
namespace {

constexpr unsigned states = 512;
constexpr unsigned paths_per_state = 4;
constexpr unsigned laps = 2;
constexpr unsigned max_hypotheses = 4;
constexpr unsigned reserved_bit_index = 32;  // information bit 33, zero-based

using Observation = std::array<std::array<std::complex<float>, 4>, fec_data_symbols>;

// Public tail-biting code (protocol.md): register = previous nine bits (newest
// low) followed by the new bit; generator outputs (1167, 1545) octal map
// 00, 01, 11, 10 to tones 0, 1, 2, 3.
unsigned tone_for(unsigned state, unsigned bit) {
    const unsigned reg = (state << 1U) | bit;
    const unsigned first = static_cast<unsigned>(std::popcount(reg & 01167U) & 1);
    const unsigned second = static_cast<unsigned>(std::popcount(reg & 01545U) & 1);
    constexpr std::array<unsigned, 4> pair_to_tone = {0, 1, 3, 2};
    return pair_to_tone[(first << 1U) | second];
}

unsigned advance(unsigned state, unsigned bit) { return ((state << 1U) | bit) & (states - 1U); }

struct Block {
    unsigned first{};   // zero-based first information bit
    unsigned length{};  // bits in the block
};

std::vector<Block> blocks_for(unsigned block_length) {
    std::vector<Block> blocks;
    for (unsigned first = 0; first < fec_data_symbols; first += block_length) {
        blocks.push_back({first, std::min(block_length, fec_data_symbols - first)});
    }
    return blocks;
}

// Branch metric |sum O_k(m)|^2 / L for every (predecessor, input word) of one
// block; the input word's most significant bit is the block's first bit.
std::vector<float> branch_table(const Observation& o, const Block& block) {
    const unsigned words = 1U << block.length;
    std::vector<float> table(static_cast<std::size_t>(states) * words);
    for (unsigned from = 0; from < states; ++from) {
        for (unsigned word = 0; word < words; ++word) {
            unsigned state = from;
            std::complex<float> sum{};
            for (unsigned q = 0; q < block.length; ++q) {
                const unsigned bit = (word >> (block.length - 1U - q)) & 1U;
                sum += o[block.first + q][tone_for(state, bit)];
                state = advance(state, bit);
            }
            table[static_cast<std::size_t>(from) * words + word] =
                std::norm(sum) / static_cast<float>(block.length);
        }
    }
    return table;
}

struct Path {
    float metric{};
    std::uint16_t origin{};
    std::uint64_t bits{};  // recorded bits of this lap, first bit most significant
};

// Ranking inside a state: descending metric, then smaller origin, then smaller
// recorded bits.
bool path_before(const Path& a, const Path& b) {
    if (a.metric != b.metric) return a.metric > b.metric;
    return std::tie(a.origin, a.bits) < std::tie(b.origin, b.bits);
}

struct Word {
    std::uint64_t bits{};  // 46 bits, information bit 1 most significant
    float accumulated{};
    float rescore{};
    unsigned start_state{};
};

InfoBits info_from(std::uint64_t bits) {
    InfoBits info{};
    for (unsigned i = 0; i < fec_data_symbols; ++i) {
        info[i] = static_cast<std::uint8_t>((bits >> (fec_data_symbols - 1U - i)) & 1U);
    }
    return info;
}

// Ranking value with information bit b as numeric bit b-1.
std::uint64_t reversed_value(std::uint64_t bits) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < fec_data_symbols; ++i) {
        value |= ((bits >> (fec_data_symbols - 1U - i)) & 1U) << i;
    }
    return value;
}

bool crc_valid(const InfoBits& info) {
    std::uint16_t reg = 0;
    for (const auto bit : info) {
        reg ^= static_cast<std::uint16_t>(bit) << 11U;
        reg = (reg & 0x800U) != 0U ? static_cast<std::uint16_t>((reg << 1U) ^ 0x80FU)
                                  : static_cast<std::uint16_t>(reg << 1U);
        reg &= 0xFFFU;
    }
    return reg == 0;
}

std::vector<Word> list_viterbi(const Observation& o, unsigned block_length) {
    const auto blocks = blocks_for(block_length);
    std::vector<std::vector<float>> tables;
    tables.reserve(blocks.size());
    for (const auto& block : blocks) tables.push_back(branch_table(o, block));

    // Up to four ranked paths per state, kept by insertion (best first).
    struct StatePaths {
        std::array<Path, paths_per_state> path{};
        unsigned count{};
        [[nodiscard]] const Path* begin() const { return path.data(); }
        [[nodiscard]] const Path* end() const { return path.data() + count; }
        Path* begin() { return path.data(); }
        Path* end() { return path.data() + count; }
        void erase_at(unsigned i) {
            for (unsigned k = i + 1; k < count; ++k) path[k - 1] = path[k];
            --count;
        }
        void insert(const Path& candidate, bool merge) {
            if (merge) {
                // Equal (origin, recorded bits): keep the larger metric (an
                // evicted equal key was better than every kept path, so a
                // worse copy cannot enter either).
                for (unsigned i = 0; i < count; ++i) {
                    if (path[i].origin == candidate.origin && path[i].bits == candidate.bits) {
                        if (!(candidate.metric > path[i].metric)) return;
                        erase_at(i);
                        break;
                    }
                }
            }
            unsigned at = count;
            while (at > 0 && path_before(candidate, path[at - 1])) --at;
            if (at >= paths_per_state) return;
            const unsigned last = std::min(count, paths_per_state - 1);
            for (unsigned k = last; k > at; --k) path[k] = path[k - 1];
            path[at] = candidate;
            count = std::min(count + 1, paths_per_state);
        }
    };
    std::vector<StatePaths> current(states);
    for (unsigned s = 0; s < states; ++s) {
        current[s].path[0] = Path{0.0F, static_cast<std::uint16_t>(s), 0};
        current[s].count = 1;
    }
    std::vector<StatePaths> next(states);

    for (unsigned lap = 0; lap < laps; ++lap) {
        for (unsigned s = 0; s < states; ++s) {
            for (auto& path : current[s]) {
                path.origin = static_cast<std::uint16_t>(s);
                path.bits = 0;
            }
        }
        for (std::size_t j = 0; j < blocks.size(); ++j) {
            const auto& block = blocks[j];
            const unsigned words = 1U << block.length;
            const unsigned keep_low = 9U - block.length;
            const bool has_reserved = reserved_bit_index >= block.first &&
                                      reserved_bit_index < block.first + block.length;
            const unsigned reserved_mask =
                has_reserved ? 1U << (block.length - 1U - (reserved_bit_index - block.first)) : 0U;
            const auto& table = tables[j];
            const bool merge = j == 0;
            for (unsigned e = 0; e < states; ++e) {
                const unsigned word = e & (words - 1U);
                auto& kept = next[e];
                kept.count = 0;
                if ((word & reserved_mask) != 0U) continue;
                const unsigned low = e >> block.length;
                for (unsigned high = 0; high < words; ++high) {
                    const unsigned from = low | (high << keep_low);
                    const float branch = table[static_cast<std::size_t>(from) * words + word];
                    for (const auto& path : current[from]) {
                        kept.insert({path.metric + branch, path.origin, (path.bits << block.length) | word}, merge);
                    }
                }
            }
            std::swap(current, next);
        }
    }

    // Lap consistency and the word set.
    std::vector<Word> words;
    for (unsigned s = 0; s < states; ++s) {
        for (const auto& path : current[s]) {
            if (path.origin != s) continue;
            auto found = std::find_if(words.begin(), words.end(),
                                      [&path](const Word& w) { return w.bits == path.bits; });
            if (found == words.end()) {
                words.push_back({path.bits, path.metric, 0.0F, s});
            } else if (path.metric > found->accumulated ||
                       (path.metric == found->accumulated && s < found->start_state)) {
                found->accumulated = path.metric;
                found->start_state = s;
            }
        }
    }
    // Circular rescore from each word's own tail-biting start state.
    for (auto& word : words) {
        const unsigned start = static_cast<unsigned>(word.bits & (states - 1U));
        word.start_state = start;
        unsigned state = start;
        float total = 0.0F;
        for (std::size_t j = 0; j < blocks.size(); ++j) {
            const auto& block = blocks[j];
            const unsigned length_mask = (1U << block.length) - 1U;
            const unsigned input = static_cast<unsigned>(
                (word.bits >> (fec_data_symbols - block.first - block.length)) & length_mask);
            total += tables[j][static_cast<std::size_t>(state) * (1U << block.length) + input];
            for (unsigned q = 0; q < block.length; ++q) {
                state = advance(state, (input >> (block.length - 1U - q)) & 1U);
            }
        }
        word.rescore = total;
    }
    std::sort(words.begin(), words.end(), [](const Word& a, const Word& b) {
        if (a.rescore != b.rescore) return a.rescore > b.rescore;
        const auto va = reversed_value(a.bits);
        const auto vb = reversed_value(b.bits);
        if (va != vb) return va < vb;
        if (a.start_state != b.start_state) return a.start_state < b.start_state;
        return a.accumulated > b.accumulated;
    });
    if (words.size() > max_hypotheses) words.resize(max_hypotheses);
    return words;
}

}  // namespace

FecOutcome decode_tiers(const SoftObservations& observations) {
    FecOutcome outcome;
    Observation split{};
    for (unsigned m = 0; m < fec_data_symbols; ++m) {
        for (unsigned k = 0; k < 4; ++k) split[m][k] = {observations.split[m][k], 0.0F};
    }
    struct Tier {
        const Observation* source;
        unsigned block_length;
    };
    const std::array<Tier, 4> tiers = {{{&observations.full, 1}, {&observations.full, 2},
                                        {&observations.full, 4}, {&split, 1}}};
    for (unsigned t = 0; t < tiers.size(); ++t) {
        const auto words = list_viterbi(*tiers[t].source, tiers[t].block_length);
        for (const auto& word : words) {
            ++outcome.crc_hypotheses;
            const auto info = info_from(word.bits);
            if (!crc_valid(info)) continue;
            PayloadBits payload{};
            std::copy_n(info.begin(), payload.size(), payload.begin());
            if (std::all_of(payload.begin(), payload.end(), [](std::uint8_t b) { return b == 0; })) {
                break;  // the tier rejects; lower-ranked words are not considered
            }
            outcome.payload = payload;
            outcome.tier = t + 1;
            return outcome;
        }
    }
    return outcome;
}

}  // namespace jtty::detail
