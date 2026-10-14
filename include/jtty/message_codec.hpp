#pragma once

#include "jtty/protocol.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jtty {

inline constexpr std::string_view message_behavior_revision =
    "wsjtx-3.3.0-beta1-message-v1";
// pack_literal_message normalizes text (upper case, single spaces, unsupported
// characters become '#') and cuts it to this many characters.
inline constexpr std::size_t max_message_bytes = 80;
inline constexpr std::size_t max_message_frames = 16;

enum class MessageProfile { unknown, field_day, rtty_roundup };
enum class AtomType {
    call,
    number,
    location,
    zone_loc3,
    class_section,
    number_time,
    control,
    grid4,
    text5,
};

// A bounded, transport-neutral typed atom. Only fields belonging to `type` are
// read; constructors at JSON or UI boundaries are expected to map their named
// fields into this representation and the codec validates every value.
struct MessageAtom {
    AtomType type{};
    std::string subtype;
    bool full_role{};
    std::uint32_t value{};
    std::uint32_t secondary_value{};
    std::string text;
};

struct MessageFrame {
    std::string atom;
    std::string subtype;
    std::string canonical_rendering;
    EncodedFrame wire;
};

struct EncodedMessage {
    std::string canonical_text;
    std::vector<MessageFrame> frames;
};

[[nodiscard]] Result<EncodedMessage> pack_literal_message(std::string_view text,
                                                          MessageProfile profile);
[[nodiscard]] Result<EncodedMessage> pack_typed_message(std::span<const MessageAtom> atoms);
[[nodiscard]] Result<EncodedMessage> decode_message_payloads(
    std::span<const PayloadBits> payloads);

// One frame's grammar validation in isolation (receiver use): the reserved bit
// must be zero and the 32-bit word must decode under the frame's own EOM bit.
struct DecodedFrameAtom {
    std::string atom;       // "TEXT5" or a structured family name
    std::string rendering;  // canonical rendering (a final TEXT5 is right-trimmed)
    std::string text5_raw;  // TEXT5 only: the five transmitted characters verbatim
    bool end_of_message{};
};
[[nodiscard]] Result<DecodedFrameAtom> decode_frame_payload(const PayloadBits& payload);

// Encodes the complete frame pipeline for an already validated 34-bit payload.
[[nodiscard]] Result<EncodedFrame> encode_payload(const PayloadBits& payload);

// Generates the bounded normative waveform for a complete message.
[[nodiscard]] Result<std::vector<float>> modulate_message(
    std::span<const MessageFrame> frames, double base_frequency_hz, double amplitude);

// The same complete-message waveform synthesized directly at 48 kHz.
[[nodiscard]] Result<std::vector<float>> modulate_message_48k(
    std::span<const MessageFrame> frames, double base_frequency_hz, double amplitude);

// Retained for callers that need independently enveloped frame artifacts. This
// is not the composed-message transmit path.
[[nodiscard]] Result<std::vector<std::vector<float>>> modulate_message_frames(
    std::span<const MessageFrame> frames, double base_frequency_hz, double amplitude);

}  // namespace jtty
