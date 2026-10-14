# jtty

A small C++20 library for **JTTY**, a text-over-audio digital mode: it encodes text into audio and decodes every message found in a recording. It has no dependencies, does no I/O and keeps no global state, so it can sit behind any audio source.

- **Transmit:** text (up to 80 characters, 1 to 16 frames) to 12 kHz audio at any base frequency from 200 to 5000 Hz.
- **Receive:** a finite 12 kHz mono recording (up to 120 s) to a list of messages with text, frequency and start time. Weak signals, several stations at once, and recordings that begin in the middle of a message are handled.
- **Bounded and deterministic:** CPU, memory and retries are capped, identical input gives identical output, and concurrent calls on different recordings are independent.

JTTY is a digital mode of [WSJT-X](https://wsjt.sourceforge.io/) 3.3.0-beta1. This library is an independent implementation: it contains no WSJT-X source code and does not need WSJT-X. See [Credits and provenance](#credits-and-provenance). It is not affiliated with or endorsed by the WSJT-X project.

## Use it

```cpp
#include <jtty/message_codec.hpp>
#include <jtty/receiver.hpp>

// Transmit: text -> audio (12 kHz, float samples).
auto message = jtty::pack_literal_message("K1ABC HELLO", jtty::MessageProfile::unknown);
auto audio = jtty::modulate_message(message.value.frames, /*base_frequency_hz=*/1500.0, /*amplitude=*/0.5);

// Receive: audio -> messages. `samples` is any mono 12 kHz float recording in [-1, 1].
auto report = jtty::receive_messages(samples);
if (!report) { /* report.error says why: empty, too long, non-finite or over the work budget */ }
for (const auto& m : report.value.results) {
    // m.text, m.frequency_hz, m.start_sample_12000 (can be negative: it began before the recording)
}
```

Every function returns `jtty::Result<T>`: check it (`if (result)`), then use `.value`; on failure `.error` holds a one-line reason and `.value` is empty.

Command-line examples are built too:

```sh
jtty_encode "K1ABC HELLO" hello.wav 1700    # text -> mono 12 kHz 16-bit WAV
jtty_decode hello.wav                       # prints: 0.500 1700 K1ABC HELLO
jtty_decode hello.wav --json
```

## Build, test, install

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
cmake --install build --prefix /your/prefix
```

Then in your project:

```cmake
find_package(jtty REQUIRED)
target_link_libraries(your_target PRIVATE jtty::jtty)
```

or add the directory with `add_subdirectory(jtty)`. Options: `JTTY_BUILD_EXAMPLES`, `JTTY_BUILD_TESTS`, `JTTY_WERROR` (all but the last default to ON). The library needs a C++20 compiler (tested with Apple clang on macOS). The receive code is compiled with `-O2` even in unoptimized builds, because a debug-speed receiver would exceed its time budget.

## Behaviour to know about

- **Input:** mono, 12 kHz, `float` samples of at most 1,440,000 (120 s). Resample and convert before calling; the library never reads files.
- **Frequency range:** 200 to 3000 Hz. WSJT-X itself receives only about 1200 to 1800 Hz with default settings; the rest is this library's extension and is a little less sensitive for long messages.
- **Text is normalized** when packed (upper case, single spaces, characters outside the JTTY alphabet become `#`) and cut to 80 characters.
- **Partial messages:** a message whose first frames were missed is returned as decoded (for example only `HELLO` of `K1ABC HELLO`); missing middle frames are marked with ` ... `, as WSJT-X does.
- **Work limits:** a reception that needs more than 2,048 window evaluations, 4,096 decoded frames or 256 messages returns an error instead of a partial result. Realistic busy recordings (two stations for 120 s, five for 60 s, ten for 30 s) decode in under 15 s each. Split longer or denser recordings.
- **A message that was never sent** is rarely published from noise or from a weak signal: about 1 in 2,000 pure-noise recordings and 1 in 1,700 weak-signal recordings. Treat decoded text like any radio copy.

## Performance

Measured with this code against WSJT-X 3.3.0-beta1 and against simulated noise; details and method are in [docs/performance.md](docs/performance.md).

| Measurement | Result |
|---|---|
| Whole message decoded at -12 dB (1 to 5 frames, 300 to 2800 Hz) | at least 99.5% |
| One- and two-frame messages at -14 dB | 90 to 98% |
| 50% decode point | -15.9 to -14.4 dB, within 0.4 dB of WSJT-X |
| Same audio through WSJT-X and this library (310 recordings) | identical text in 98.4% |
| Text that was not transmitted: weak signals (5,000) / pure noise (20,000) | 0.06% / 0.045% |

(SNR is signal power over noise in a 2500 Hz bandwidth.) Real on-air recordings have not been tested.

## Credits and provenance

**JTTY** was created by its authors as part of the **WSJT-X** project ([wsjt.sourceforge.io](https://wsjt.sourceforge.io/)), which is developed by Joe Taylor, K1JT, and the WSJT Development Group and released under the GNU General Public License. The mode's frame format, error-correcting code, synchronization word and message grammar, and the receiver design that this library follows, are their work, published in the `lib/jtty` directory of the WSJT-X 3.3.0-beta1 source release (`jtty_design.md`, `jtty_source_encoding.txt`). Thank you to the WSJT-X developers and to the authors of the JTTY mode. <!-- Add the JTTY authors' names and callsigns here. -->

**How this library was written.** It was developed under a source-separation ("clean-room") process:

- The implementer worked from the public JTTY documentation and from written technical specifications of the receive path. The specifications were prepared by a separate reviewer who had read the WSJT-X source; according to the project's provenance records the implementer of the receiver did not read the WSJT-X receiver source, patches or traces.
- WSJT-X was used only as a black-box reference: the same audio was decoded by WSJT-X and by this library and the results compared (see [docs/performance.md](docs/performance.md)). No WSJT-X code is included in or linked to this library.
- Where this library differs from WSJT-X (the 200 to 3000 Hz search range, the handling of recordings that start inside a frame, an extra plausibility test on decoded words) the difference is documented in the code and above.

The process was not perfect: in the final stage a few lines of non-decoder WSJT-X GUI code and build patches were viewed by the same person who then adjusted receiver parameters (the adjustments were derived from measurements, not from that code). This statement is made in good faith and is not legal advice; if the provenance matters for your use, review the code and the process yourself.

## License

MIT, see [LICENSE](LICENSE). Copyright (c) 2026 Jouni OH3CUF. WSJT-X is a separate work under the GPL; this library neither includes nor depends on it.
