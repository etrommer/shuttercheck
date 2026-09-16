#!/usr/bin/env python3
"""Generate test/test_data.h for the shutter edge scan (issue 5).

Builds each synthetic sample stream and writes its hand-specified expected
result sequence into a C header that the native Unity test links and scans
against. Standard library only, deterministic. No scan simulation: the
expected values come from the fixture geometry and the spec, not from running
the firmware algorithm.

Every clean pulse uses a single-sampling-step leading and trailing edge. For
such a pulse the measured exposure equals the plateau sample count times the
2.000 us period exactly, at any operating threshold, because the two edges
are congruent. So the width is hand-specified as `plateau * PERIOD_NS` and the
test genuinely checks the firmware reproduces it.

Run as a PlatformIO pre-script for the native environment so the data is
regenerated before every `pio test -e native`; the generated header is not
checked in.
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
# Each entry: (stream, [ (width_ns, status), ... ]).

CASES = []

# 0. Clean pulse at 1/1000. Exact width from the plateau geometry.
CASES.append((pulse(100, 2500, 500, 256, 64), [(ns(500), "ok")]))

# 1. Clean pulse at 1/4000: few samples per edge.
CASES.append((pulse(100, 2500, 125, 256, 64), [(ns(125), "ok")]))

# 2. Crossing pair straddles the half-buffer boundary (sample 512). The bright
#    block occupies samples 400..579, so the rising edge is scanned in chunk 0
#    and the falling edge in chunk 1. The width must stay correct.
CASES.append((pulse(100, 2500, 180, 400, 60), [(ns(180), "ok")]))

# 3. Saturated bright plateau => clipped. The bright plateau must stay near
#    full scale long enough for the tracked estimate to rise to within 32 LSB
#    of it; 500 samples at K=64 does.
CASES.append((pulse(100, 4080, 500, 256, 64), [(0, "clipped")]))

# 4. Bright-dark span below 48 LSB => weak.
CASES.append((pulse(100, 120, 200, 256, 64), [(0, "weak")]))

# 5. A lone falling crossing without a rising crossing => stale. The signal
#    sits in the hysteresis band (106: below the boot threshold 108, above the
#    low band edge 104), so the jump up to bright does not re-arm and the jump
#    does not register as a rising edge. The subsequent fall back to dark has
#    no rising edge of its own: stale.
CASES.append((plateau(100, 8) + plateau(106, 8) + plateau(2500, 16)
              + plateau(100, 8), [(0, "stale")]))

# 6. Noise inside the hysteresis band: no extra crossing, no re-arm. The 200
#    sample bright plateau carries a few dips down into the hysteresis band
#    (values about the saturated midpoint, i.e. above the locked threshold).
#    The pulse still closes exactly once, with the plateau width intact.
bw = [2500] * 200
for k in (30, 80, 130, 170):
    bw[k] = 1300                      # a dip inside the band, not a real edge.
CASES.append((plateau(100, 256) + bw + plateau(100, 64), [(ns(200), "ok")]))

# 7. A second pulse after a full re-arm => the firmware measures both.
CASES.append((pulse(100, 2500, 150, 256, 128) + pulse(100, 2500, 150, 0, 64),
              [(ns(150), "ok"), (ns(150), "ok")]))


def emit_case_header(f, idx, seq, res):
    f.write("// Case %d: %d samples -> %d result(s).\n"
            % (idx, len(seq), len(res)))
    f.write("extern const uint16_t kCase%dSamples[%d];\n" % (idx, len(seq)))
    f.write("extern const testdata::Expected kCase%dExpected[%d];\n"
            % (idx, len(res)))


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
        f.write("  const Expected* expected;\n")
        f.write("  uint32_t expectedCount;\n")
        f.write("};\n")
        f.write("\n")
        for idx, (seq, res) in enumerate(CASES):
            emit_case_header(f, idx, seq, res)
            f.write("\n")
        for idx, (seq, res) in enumerate(CASES):
            f.write("const uint16_t kCase%dSamples[%d] = {\n  "
                    % (idx, len(seq)))
            f.write(", ".join(str(x) for x in seq))
            f.write("\n};\n\n")
            f.write("const testdata::Expected kCase%dExpected[%d] = {\n  "
                    % (idx, len(res)))
            f.write(", ".join("{%d, \"%s\"}" % (w, s) for w, s in res))
            f.write("\n};\n\n")
        f.write("const Case kCases[%d] = {\n" % len(CASES))
        for idx, (seq, res) in enumerate(CASES):
            f.write("  { kCase%dSamples, %d, kCase%dExpected, %d },\n"
                    % (idx, len(seq), idx, len(res)))
        f.write("};\n")
        f.write("const uint32_t kNumCases = %d;\n" % len(CASES))
        f.write("\n}  // namespace testdata\n")
        f.write("\n#endif  // SHUTTERCHECK_TEST_DATA_H\n")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "test", "test_data.h")
    write_header(os.path.normpath(out))
    total = sum(len(res) for _, res in CASES)
    print("gen_test_data: %d cases, %d expected results -> %s"
          % (len(CASES), total, os.path.normpath(out)))


if __name__ == "__main__":
    main()