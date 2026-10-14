#pragma once

// Internal message assembly of the receiver (design notes
// sections 11 and 13.1 c-d to 13.6). Pure timing/frequency logic over
// validated frame entries; no signal processing. Not installed.

#include "jtty/protocol.hpp"
#include "jtty/receiver.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace jtty::detail {

inline constexpr double assembly_rate_hz = 12'000.0;
inline constexpr double frame_period_s = static_cast<double>(samples_per_frame) / assembly_rate_hz;  // 1.888 s
inline constexpr double step_period_s = frame_period_s / 4.0;                                       // 0.472 s
inline constexpr double stamp_df_hz = 12.0;
inline constexpr double stamp_dt_s = 0.05;
inline constexpr double alias_df_hz = 10.0;
inline constexpr double alias_residual_s = 0.003;
inline constexpr double continuation_residual_s = 0.1;
inline constexpr char stand_in = '\x01';  // internal stand-in for a TEXT5 space (section 11)

struct FrameEntry {
    double frequency_hz{};
    double start_12k{};  // emitted-stream sample position of the frame start
    PayloadBits payload{};
    std::string text;    // frame text with TEXT5 spaces as stand-ins
    bool space_after{};
    bool eom{};
    [[nodiscard]] double seconds() const { return start_12k / assembly_rate_hz; }
};

struct OpenMessage {
    std::string text;
    bool space_after{};
    double f_last{};
    double t_last{};  // seconds
    FrameEntry first;
    std::size_t frames{};
};

enum class MergeResult {
    stamp_duplicate,   // section 13.1 c
    alias_duplicate,   // section 13.1 d
    started_open,
    started_complete,  // a starting frame with EOM: complete at once
    refused,           // 30 messages already open and the frame has no EOM
    appended,
    completed,
    history_full,      // the retained-frame bound would be exceeded (fail closed)
};

// Positional open list, stored frame stamps and frame history for one
// reception. Counters go to the caller's ReceiveWork (duplicate_frames,
// messages_started, incomplete_chains).
class Assembler {
public:
    using Completion = std::function<void(const OpenMessage&)>;
    Assembler(ReceiveWork& work, std::size_t history_limit, Completion on_complete);

    MergeResult merge(const FrameEntry& entry);
    // Section 13.5, before every forward step whose window starts at
    // `window_seconds`: forget old stamps and time out stale open messages.
    void expire(double window_seconds);
    // End of reception: every open message is cut off. Returns their number.
    std::size_t finish();

    [[nodiscard]] const std::vector<OpenMessage>& open() const { return open_; }
    [[nodiscard]] const std::vector<FrameEntry>& history() const { return history_; }
    [[nodiscard]] std::size_t stamps() const { return stamps_.size(); }

private:
    void remove_open(std::size_t i);
    void stamp(const FrameEntry& entry);

    ReceiveWork& work_;
    std::size_t history_limit_;
    Completion on_complete_;
    std::vector<OpenMessage> open_;
    std::deque<std::pair<double, double>> stamps_;  // (frequency, seconds)
    std::vector<FrameEntry> history_;
};

// Within-window duplicate tests of section 13.1 a and b for one window
// evaluation, and the channel-Rx decode records (at most 16) that also drive
// the section 6 vicinity zeroing.
class WindowDuplicates {
public:
    enum class Verdict { fresh, same_text, rx_repeat };
    struct Record {
        double frequency_hz{};
        double seconds{};
    };
    static constexpr double same_text_dt_s = 0.032;
    static constexpr double rx_repeat_df_hz = 3.0;
    static constexpr double rx_repeat_dt_s = 0.05;
    static constexpr std::size_t max_rx_records = 16;

    // a: identical text within 32 ms, any frequency; b (fixed channels only):
    // a channel-Rx decode within 3 Hz and 50 ms.
    [[nodiscard]] Verdict test(const std::string& text, double frequency_hz, double seconds,
                               bool fixed_channel) const;
    void add_decoded(const std::string& text, double seconds) { decoded_.emplace_back(text, seconds); }
    void add_rx(double frequency_hz, double seconds) {
        if (rx_.size() < max_rx_records) rx_.push_back({frequency_hz, seconds});
    }
    [[nodiscard]] const std::vector<Record>& rx() const { return rx_; }

private:
    std::vector<std::pair<std::string, double>> decoded_;
    std::vector<Record> rx_;
};

// Section 13.6 publication text followed by the display/export normalization.
std::string publication_text(const std::string& internal);

}  // namespace jtty::detail
