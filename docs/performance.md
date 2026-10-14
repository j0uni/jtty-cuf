# Measured performance

Measured on 2026-10-14. SNR is signal power over Gaussian noise in a 2500 Hz
bandwidth. Each trial places the message at a random start within one frame in
12,000 Hz noise with a fixed seed; "full" is the complete transmitted text,
"partial" is the text with missing frames marked ` ... ` or only its tail frames,
"wrong" is text that was not transmitted.

## Sensitivity (200 trials per cell)

| message | frames | Hz | -12 dB full | -14 dB full | -14 dB partial | -14 dB nothing |
|---|---:|---:|---:|---:|---:|---:|
| HELLO | 1 | 300 | 100.0% | 93.5% | 0.0% | 6.5% |
| K1ABC HELLO | 2 | 300 | 100.0% | 90.5% | 6.0% | 3.5% |
| CQ K1ABC FN42 | 3 | 300 | 99.5% | 79.0% | 13.0% | 8.0% |
| HELLO, WORLD! 73 DE K1ABC | 5 | 300 | 99.5% | 69.5% | 27.0% | 3.5% |
| HELLO | 1 | 1500 | 100.0% | 97.5% | 0.0% | 2.5% |
| K1ABC HELLO | 2 | 1500 | 100.0% | 94.5% | 2.5% | 3.0% |
| CQ K1ABC FN42 | 3 | 1500 | 100.0% | 88.0% | 8.5% | 3.5% |
| HELLO, WORLD! 73 DE K1ABC | 5 | 1500 | 100.0% | 78.0% | 19.0% | 3.0% |
| HELLO | 1 | 2800 | 100.0% | 98.0% | 0.0% | 2.0% |
| K1ABC HELLO | 2 | 2800 | 100.0% | 90.0% | 5.0% | 5.0% |
| CQ K1ABC FN42 | 3 | 2800 | 100.0% | 81.5% | 13.0% | 5.5% |
| HELLO, WORLD! 73 DE K1ABC | 5 | 2800 | 100.0% | 64.5% | 30.0% | 5.5% |

Longer messages lose frames near the limit; WSJT-X loses the same ones (it
completes a five-frame message in about 65% of trials at 1500 Hz and -14 dB, this
library in 75%, 20 trials). Outside 1200 to 1800 Hz there is no WSJT-X result to
compare with.

## Comparison with WSJT-X 3.3.0-beta1

310 recordings were decoded by WSJT-X (a pinned build run in a hidden window) and by
this library: one-, two- and three-frame messages at 1500 and 1700 Hz and -13, -15
and -17 dB (15 trials per cell), 20 recordings with two signals 40 to 100 Hz apart
(one 6 or 10 dB stronger), and 20 recordings that begin or end inside a message.

- The decoded text is identical in 305 of 310 recordings (98.4%). The five
  differences are single recordings at -15 and -17 dB where one frame decides
  between a full, a partial and no result.
- The library never published text that WSJT-X did not also publish and that was
  not part of a transmitted message.
- The largest difference in complete-decode probability in any cell is 6.7
  percentage points (one trial of 15).
- The 50% decode points agree within 0.4 dB: HELLO 1500 Hz -15.6 / -15.9 dB
  (WSJT-X / this library), K1ABC HELLO 1500 Hz -15.5 / -15.4, CQ K1ABC FN42 1500 Hz
  -15.0 / -15.2, and the same three at 1700 Hz -14.9 / -15.3, -14.9 / -14.7,
  -14.3 / -14.4.
- A recording that starts inside a message is decoded the same way: the whole
  message when up to about 0.25 s (3,000 samples) of its start is missing, only the
  tail frames beyond that.

## Text that was not transmitted

- 5,000 weak-signal recordings (-14 to -18 dB): 3 published text that was not
  transmitted (0.06%).
- 20,000 recordings of pure noise: 9 published a short message (0.045%, 95% upper
  bound 0.086%). A stricter acceptance test removes most of them at some cost in
  sensitivity near -17 dB; it is the first candidate for a later improvement.

## Work and resources

A reception is limited to 2,048 window evaluations, 4,096 decoded frames and 256
messages; exceeding a limit returns an error. Timing on a development laptop, one
core: two stations for 120 s decode in 7 s, five for 60 s in 8 s, ten for 30 s in 9
s, four stations for 120 s in 14 s. A pathological recording with 28 simultaneous
stations for 120 s fails with an error within about 100 s. Peak memory stays under
40 MB. Under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer the
code is clean, and output is byte-identical between runs and between `-O0` and
`-O2` builds.

## Not measured

Real on-air recordings (fading, interference, other modes), message types beyond
the four above, and statistics beyond the sample sizes shown.
