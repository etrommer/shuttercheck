#!/usr/bin/env python3
"""Generate test/test_data.h for the shutter edge scan (issue 5, issue 10).

Each case is one synthetic stream plus the result list the scan must produce
under each signal polarity (issue 10):

- kDarkLow runs the stream as written, with the low plateau as the dark one,
- kDarkHigh runs the mirrored stream, `v -> dark + bright - v`, which exchanges
  the two plateaus. That is what the inverting cascode front end does to the
  signal, so the same case covers both front ends.

The exposure of a step-edge pulse is the same in both directions: the two
crossings interpolate the same fraction between the same pair of plateau
values, so the fractions cancel in the difference.

The expected values are literals derived from the spec geometry (crossing
formula, known thresholds); nothing here replicates the scan algorithm.
"""

import os

PERIOD_NS = 2000      # 2.000 us per sample == 500 kS/s.
CHUNK = 512           # One DMA buffer half (== scan chunk).


def plateau(value, n):
    return [value] * n


def pulse(dark, bright, width, lead, tail):
    """A single-step-edge pulse: `lead` dark samples, `width` bright samples,
    `tail` dark samples. Exposure == width * PERIOD_NS exactly."""
    return plateau(dark, lead) + [bright] * width + plateau(dark, tail)


def ns(width):
    return width * PERIOD_NS


# ---- Test cases (issue 5, "Unit test cases (synthetic data)") ----
# Each entry: (low, high, stream, [kDarkLow results], [kDarkHigh results]).
# The stream as written always starts on its low plateau, because that is the
# dark plateau under kDarkLow. Its mirror starts on the high plateau, which is
# the dark plateau under kDarkHigh.
# The default kDarkHigh result list is the kDarkLow one when the polarity does
# not change the verdict; pass an explicit list where it does.

CASES = []


def case(low, high, seq, res, res_high=None):
    CASES.append((low, high, seq, res, res_high if res_high else res))


# 0. Clean pulse at 1/1000. Exact width from the plateau geometry.
case(100, 2500, pulse(100, 2500, 500, 256, 64), [(ns(500), "ok")])

# 1. Clean pulse at 1/4000: few samples per edge.
case(100, 2500, pulse(100, 2500, 125, 256, 64), [(ns(125), "ok")])

# 2. Crossing pair straddles the half-buffer boundary (sample 512). The bright
#    block occupies samples 400..579, so the rising edge is scanned in chunk 0
#    and the falling edge in chunk 1. The width must stay correct.
case(100, 2500, pulse(100, 2500, 180, 400, 60), [(ns(180), "ok")])

# 3. The lit plateau is at the ADC rail. Under kDarkLow the firmware tracks it
#    as the bright plateau and latches the rail flag from the samples inside
#    the open pulse. Under kDarkHigh the mirrored stream puts the rail plateau
#    on the dark side, which is sampled while the shutter is closed, before the
#    pulse opens. Both are clipped: the plateau that the front end can drive to
#    full scale is saturated either way.
case(100, 4080, pulse(100, 4080, 500, 256, 64), [(0, "clipped")])

# 4. Plateau span below 48 LSB => weak.
case(100, 120, pulse(100, 120, 200, 256, 64), [(0, "weak")])

# 5. A signal that rests in the hysteresis band and then crosses the threshold.
#    The last sample before the jump (106) is inside the band: above the low
#    band edge (104) and below the boot threshold (108). A crossing is a
#    threshold straddle (issue 26), so the jump to bright opens a pulse and the
#    return to dark closes it. The result is one measured pulse, not a lone
#    crossing. The two polarities interpolate the straddle with a small
#    rounding difference.
case(100, 2500, plateau(100, 8) + plateau(106, 8) + plateau(2500, 16)
     + plateau(100, 8), [(32005, "ok")], [(31995, "ok")])

# 6. Noise inside the hysteresis band: no extra crossing, no re-arm. The 200
#    sample bright plateau carries a few dips down into the hysteresis band
#    (values about the saturated midpoint, i.e. above the locked threshold).
#    The pulse still closes exactly once, with the plateau width intact.
bw = [2500] * 200
for k in (30, 80, 130, 170):
    bw[k] = 1300                      # a dip inside the band, not a real edge.
case(100, 2500, plateau(100, 256) + bw + plateau(100, 64), [(ns(200), "ok")])

# 7. A second pulse after a full re-arm => the firmware measures both.
case(100, 2500, pulse(100, 2500, 150, 256, 128) + pulse(100, 2500, 150, 0, 64),
     [(ns(150), "ok"), (ns(150), "ok")])

# 8. The cascode operating point with the dark plateau at the rail: the cascode
#    output rests near full scale with the shutter closed and at V_bias with it
#    open. Written for kDarkLow, so the rail plateau (4090) is the lit one. Its
#    mirror puts the rail plateau on the dark side, sampled while the shutter is
#    closed, before the pulse opens. Both are clipped: the plateau that the
#    front end can drive to full scale is saturated either way.
case(2048, 4090, pulse(2048, 4090, 500, 256, 64), [(0, "clipped")])

# 9. The same cascode geometry with the rail plateau just outside the rail zone:
#    measured. Mirrored, the dark plateau is 4000 LSB, about 31 LSB below the
#    32 LSB rail margin.
case(2048, 4000, pulse(2048, 4000, 500, 256, 64), [(ns(500), "ok")])


def emit_case_header(f, idx, seq, res, res_high):
    f.write("// Case %d: %d samples -> %d result(s) at kDarkLow, %d at kDarkHigh.\n"
            % (idx, len(seq), len(res), len(res_high)))
    f.write("extern const uint16_t kCase%dSamples[%d];\n" % (idx, len(seq)))
    f.write("extern const testdata::Expected kCase%dExpectedLow[%d];\n"
            % (idx, len(res)))
    f.write("extern const testdata::Expected kCase%dExpectedHigh[%d];\n"
            % (idx, len(res_high)))


def write_expected(f, name, res):
    f.write("const testdata::Expected %s[%d] = {\n  " % (name, len(res)))
    f.write(", ".join("{%d, \"%s\"}" % (w, s) for w, s in res))
    f.write("\n};\n\n")


def write_header(path):
    with open(path, "w") as f:
        f.write("#ifndef SHUTTERCHECK_TEST_DATA_H\n")
        f.write("#define SHUTTERCHECK_TEST_DATA_H\n")
        f.write("\n")
        f.write("// GENERATED FILE - do not edit. Created by scripts/gen_test_data.py.\n")
        f.write("\n")
        f.write("#include <stdint.h>\n")
        f.write("\n")
        f.write("namespace testdata {\n")
        f.write("\n")
        f.write("struct Expected { int64_t ns; const char* status; };\n")
        f.write("struct Case {\n")
        f.write("  const uint16_t* samples;\n")
        f.write("  uint32_t count;\n")
        f.write("  int32_t lowPlateau;   // The plateau the stream starts on.\n")
        f.write("  int32_t highPlateau;  // The other plateau value.\n")
        f.write("  const Expected* expectedLow;\n")
        f.write("  const Expected* expectedHigh;\n")
        f.write("  uint32_t expectedCountLow;\n")
        f.write("  uint32_t expectedCountHigh;\n")
        f.write("};\n")
        f.write("\n")
        for idx, (_, _, seq, res, res_high) in enumerate(CASES):
            emit_case_header(f, idx, seq, res, res_high)
            f.write("\n")
        for idx, (_, _, seq, res, res_high) in enumerate(CASES):
            f.write("const uint16_t kCase%dSamples[%d] = {\n  "
                    % (idx, len(seq)))
            f.write(", ".join(str(x) for x in seq))
            f.write("\n};\n\n")
            write_expected(f, "kCase%dExpectedLow" % idx, res)
            write_expected(f, "kCase%dExpectedHigh" % idx, res_high)
        f.write("const Case kCases[%d] = {\n" % len(CASES))
        for idx, (low, high, seq, res, res_high) in enumerate(CASES):
            f.write("  { kCase%dSamples, %d, %d, %d, kCase%dExpectedLow, "
                    "kCase%dExpectedHigh, %d, %d },\n"
                    % (idx, len(seq), low, high, idx, idx, len(res),
                       len(res_high)))
        f.write("};\n")
        f.write("const uint32_t kNumCases = %d;\n" % len(CASES))
        f.write("// The longest stream, for the mirror buffer of the shared\n")
        f.write("// case runner (test/scan_cases.h).\n")
        f.write("constexpr uint32_t kMaxSamples = %d;\n"
                % max(len(seq) for _, _, seq, _, _ in CASES))
        f.write("\n}  // namespace testdata\n")
        f.write("\n#endif  // SHUTTERCHECK_TEST_DATA_H\n")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "test", "test_data.h")
    write_header(os.path.normpath(out))
    total = sum(len(res) + len(res_high)
                for _, _, _, res, res_high in CASES)
    print("gen_test_data: %d cases, %d expected results -> %s"
          % (len(CASES), total, os.path.normpath(out)))


if __name__ == "__main__":
    main()