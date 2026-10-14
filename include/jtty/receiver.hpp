#pragma once

#include "jtty/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jtty {

// Receiver for finite recordings of JTTY audio.
//
// The receiver follows the structure of the WSJT-X JTTY decoder, implemented
// independently from public documentation and black-box observation: a stream
// of 2.36 s analysis windows stepped by a quarter frame, a coarse sync map over
// frequency channels, refinement and sync-quality gating, tiered list Viterbi
// decoding, per-frame grammar validation, duplicate tests, in-window signal
// subtraction with look-back re-search, an expected-frame retry, and
// timing/frequency message assembly. Where this library differs (the search
// reaches 200--3000 Hz instead of about 1200--1800 Hz, recordings that begin
// inside a frame, an extra plausibility test on decoded words) the difference is
// stated at the definition.
inline constexpr std::string_view receiver_revision = "jtty-receiver-1";
// Published messages report a rounded frequency in this closed interval.
inline constexpr int receiver_search_low_hz = 200;
inline constexpr int receiver_search_high_hz = 3000;
// Receive settings of the WSJT-X JTTY decoder that the channel layout follows.
inline constexpr double receiver_rx_frequency_hz = 1500.0;
inline constexpr double receiver_tolerance_hz = 20.0;

// Work bounds. They are enforced, not advisory.
inline constexpr std::size_t receiver_max_input_samples = 1'440'000;  // 120 s
inline constexpr std::size_t receiver_max_window_candidates = 100;
inline constexpr std::size_t receiver_max_lookback_windows = 48;  // per forward window
inline constexpr std::size_t receiver_max_open_messages = 30;
inline constexpr std::size_t receiver_max_frame_stamps = 480;
inline constexpr std::size_t receiver_max_message_characters = 80;
inline constexpr std::size_t receiver_max_results = 256;
// Reception-level work budget. A
// reception that needs more fails closed as soon as the budget is exceeded,
// with no partial result: at most 2,048 window evaluations (forward plus
// look-back; a 120 s input has about 251 forward windows), at most 4,096 CRC-
// and grammar-valid decoded frames, and at most 256 completed messages. Each
// recorded subtraction costs up to three look-back evaluations; the budget
// admits about four continuously transmitting signals for 120 s of realistic
// traffic, and denser inputs fail closed.
inline constexpr std::size_t receiver_max_window_evaluations = 2048;
inline constexpr std::size_t receiver_max_decoded_frames = 4096;

enum class ReceiveOutcome { no_result, accepted_complete, multiple_result };

struct ReceivedMessage {
    std::string text;
    int frequency_hz{};
    // Estimated start of the first decoded frame, relative to input sample 0.
    // A start before the first input sample is negative.
    std::int64_t start_sample_12000{};
    std::size_t frame_count{};
};

// Deterministic work counters reported for audit; none is a receiver claim.
struct ReceiveWork {
    std::size_t input_samples{};
    std::size_t windows{};            // forward analysis windows evaluated
    std::size_t silent_windows{};     // forward or look-back windows skipped as all-zero
    std::size_t lookback_windows{};   // look-back window evaluations
    std::size_t candidates{};         // candidate records (picks and retries)
    std::size_t gated_candidates{};   // picks that passed the sync-quality gate
    std::size_t retries{};            // expected-frame retries
    std::size_t fec_hypotheses{};     // CRC checks over all tiers
    std::size_t frames_decoded{};     // CRC- and grammar-valid frames
    std::size_t duplicate_frames{};   // within-window, Rx-repeat, stamp and alias duplicates
    std::size_t subtractions{};
    std::size_t second_pass_frames{};  // new frames found in a pass 2 (after subtraction)
    std::size_t lookback_frames{};     // new frames found in look-back windows
    std::size_t messages_started{};
    std::size_t incomplete_chains{};  // timed out, cut off, or refused open messages
    std::size_t missing_leading_frame_chains{};  // always 0: kept for the result schema; suffixes are published
    std::size_t rejected_messages{};  // completed but outside the search interval
};

struct ReceiveReport {
    ReceiveOutcome outcome{ReceiveOutcome::no_result};
    // Sorted by ascending start sample, then frequency, then text bytes.
    std::vector<ReceivedMessage> results;
    ReceiveWork work;
};

[[nodiscard]] std::string_view receive_outcome_name(ReceiveOutcome outcome);

// Processes one complete finite input as one reception. Fails closed for an
// empty, oversized, or non-finite input, as soon as more than
// receiver_max_results messages complete, or when the work budget above is
// exceeded. An input that completes normally
// without a message reports ReceiveOutcome::no_result.
// Reentrant: the function keeps no global mutable state (immutable tables are
// built once, scratch is per thread), so concurrent calls on different inputs
// are independent and each result equals the sequential one.
[[nodiscard]] Result<ReceiveReport> receive_messages(std::span<const float> pcm_12k);

}  // namespace jtty
