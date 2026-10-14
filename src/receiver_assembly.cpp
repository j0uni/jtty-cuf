#include "receiver_assembly.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace jtty::detail {

Assembler::Assembler(ReceiveWork& work, std::size_t history_limit, Completion on_complete)
    : work_(work), history_limit_(history_limit), on_complete_(std::move(on_complete)) {}

void Assembler::remove_open(std::size_t i) {
    // Positional list: the last entry moves into the vacated position.
    if (i + 1 != open_.size()) open_[i] = std::move(open_.back());
    open_.pop_back();
}

void Assembler::stamp(const FrameEntry& entry) {
    if (stamps_.size() >= receiver_max_frame_stamps) stamps_.pop_front();
    stamps_.emplace_back(entry.frequency_hz, entry.seconds());
}

void Assembler::expire(double window_seconds) {
    const double r = window_seconds - 3.0 * step_period_s;
    std::erase_if(stamps_, [r](const auto& s) { return s.second < r - stamp_dt_s; });
    for (std::size_t i = 0; i < open_.size();) {
        if (r - open_[i].t_last > 3.0 * frame_period_s + continuation_residual_s) {
            ++work_.incomplete_chains;
            remove_open(i);  // the same position is tested again
        } else {
            ++i;
        }
    }
}

std::size_t Assembler::finish() {
    const std::size_t count = open_.size();
    work_.incomplete_chains += count;
    open_.clear();
    return count;
}

MergeResult Assembler::merge(const FrameEntry& entry) {
    const double t = entry.seconds();
    for (const auto& [frequency, seconds] : stamps_) {
        if (std::abs(frequency - entry.frequency_hz) < stamp_df_hz && std::abs(seconds - t) < stamp_dt_s) {
            ++work_.duplicate_frames;
            return MergeResult::stamp_duplicate;
        }
    }
    const auto continuation_gap = [&](const OpenMessage& m) -> int {
        const double dt = t - m.t_last;
        const auto j = static_cast<int>(std::lround(dt / frame_period_s));
        if (j < 1 || j > 3) return 0;
        if (!(std::abs(dt - j * frame_period_s) < continuation_residual_s)) return 0;
        if (!(std::abs(entry.frequency_hz - m.f_last) < 10.0 + 3.0 * (j - 1))) return 0;
        return j;
    };
    for (const auto& m : open_) {
        if (continuation_gap(m) != 0) continue;
        const double dt = t - m.t_last;
        const auto s = std::lround(dt / step_period_s);
        if (std::abs(s) <= 3 && std::abs(entry.frequency_hz - m.f_last) < alias_df_hz &&
            std::abs(dt - static_cast<double>(s) * step_period_s) < alias_residual_s) {
            ++work_.duplicate_frames;
            return MergeResult::alias_duplicate;
        }
    }
    std::size_t chosen = open_.size();
    int gap = 0;
    double best_df = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < open_.size(); ++i) {
        const int j = continuation_gap(open_[i]);
        if (j == 0) continue;
        const double df = std::abs(entry.frequency_hz - open_[i].f_last);
        if (df < best_df) {  // strict: the first in open-list order wins a tie
            best_df = df;
            chosen = i;
            gap = j;
        }
    }
    if (history_.size() >= history_limit_) return MergeResult::history_full;
    history_.push_back(entry);
    if (chosen == open_.size()) {
        OpenMessage m;
        m.text = entry.text;
        if (m.text.rfind("599 ", 0) == 0) m.text.insert(m.text.begin(), stand_in);
        if (m.text.size() > receiver_max_message_characters) m.text.resize(receiver_max_message_characters);
        m.space_after = entry.space_after;
        m.f_last = entry.frequency_hz;
        m.t_last = t;
        m.first = entry;
        m.frames = 1;
        if (entry.eom) {
            ++work_.messages_started;
            stamp(entry);
            on_complete_(m);
            return MergeResult::started_complete;
        }
        if (open_.size() >= receiver_max_open_messages) {
            ++work_.incomplete_chains;
            return MergeResult::refused;  // its stamp is not stored
        }
        ++work_.messages_started;
        stamp(entry);
        open_.push_back(std::move(m));
        return MergeResult::started_open;
    }
    auto& m = open_[chosen];
    if (gap == 1) {
        if (m.space_after) m.text.push_back(' ');
        m.text += entry.text;
    } else {
        m.text += std::string(5, stand_in);
        std::string text = entry.text;
        if (!text.empty() && text.front() == stand_in) text.erase(text.begin());
        m.text += text;
    }
    if (m.text.size() > receiver_max_message_characters) m.text.resize(receiver_max_message_characters);
    m.space_after = entry.space_after;
    m.f_last = entry.frequency_hz;
    m.t_last = t;
    ++m.frames;
    stamp(entry);
    if (entry.eom) {
        OpenMessage done = std::move(m);
        remove_open(chosen);
        on_complete_(done);
        return MergeResult::completed;
    }
    return MergeResult::appended;
}

WindowDuplicates::Verdict WindowDuplicates::test(const std::string& text, double frequency_hz, double seconds,
                                                  bool fixed_channel) const {
    for (const auto& [earlier, when] : decoded_) {
        if (earlier == text && std::abs(seconds - when) < same_text_dt_s) return Verdict::same_text;
    }
    if (fixed_channel) {
        for (const auto& rx : rx_) {
            if (std::abs(rx.frequency_hz - frequency_hz) < rx_repeat_df_hz && std::abs(rx.seconds - seconds) < rx_repeat_dt_s) {
                return Verdict::rx_repeat;
            }
        }
    }
    return Verdict::fresh;
}

std::string publication_text(const std::string& internal) {
    std::string text;
    for (std::size_t i = 0; i < internal.size();) {
        if (internal.compare(i, 5, std::string(5, stand_in)) == 0) {
            text += " ... ";
            i += 5;
        } else {
            text.push_back(internal[i] == stand_in ? ' ' : internal[i]);
            ++i;
        }
    }
    if (!text.empty() && text.front() == ' ') text.erase(text.begin());
    while (!text.empty() && text.back() == ' ') text.pop_back();
    // Remove idle runs (five '<' with surrounding spaces), collapse whitespace.
    for (std::size_t at = text.find("<<<<<"); at != std::string::npos; at = text.find("<<<<<")) {
        text.replace(at, 5, " ");
    }
    std::string out;
    for (const char ch : text) {
        if (ch == ' ' && (out.empty() || out.back() == ' ')) continue;
        out.push_back(ch);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace jtty::detail
