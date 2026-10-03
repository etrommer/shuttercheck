// Shared scan case runner (issue 8).
//
// Feeds one generated case from test/test_data.h through the scan in the
// 512-sample DMA chunk quantum and records what came out. It is free of Unity
// and of the HAL, so the native unit test and the on-device self test share
// one feeder and one set of expectations: only the reporter differs.
//
// Every case runs under both signal polarities (issue 10). The kDarkLow run
// feeds the generated stream, whose first plateau is the dark one. The
// kDarkHigh run feeds its mirror, which exchanges the two plateaus: that is
// what the inverting cascode front end does to the signal.
#ifndef SHUTTERCHECK_SCAN_CASES_H
#define SHUTTERCHECK_SCAN_CASES_H

#include <stdint.h>

#include "scan.h"
#include "test_data.h"

namespace scancases {

// One result, as the scan produced it.
struct Observed {
  bool present;  // A result came out for this slot.
  int64_t ns;
  const char* status;
};

// What one case produced under one polarity. Compare `got` against
// expectedAt(index, high).
struct CaseRun {
  uint32_t index;
  bool darkHigh;        // The polarity this run used.
  uint32_t expectedCount;
  uint32_t gotCount;
  Observed got[scan::kFifoSize];
};

inline uint32_t caseCount() { return testdata::kNumCases; }
inline const testdata::Case& caseAt(uint32_t idx) { return testdata::kCases[idx]; }

// The expected results of one case under one polarity.
inline const testdata::Expected* expectedAt(uint32_t idx, bool darkHigh) {
  return darkHigh ? testdata::kCases[idx].expectedHigh
                  : testdata::kCases[idx].expectedLow;
}
inline uint32_t expectedCountAt(uint32_t idx, bool darkHigh) {
  return darkHigh ? testdata::kCases[idx].expectedCountHigh
                  : testdata::kCases[idx].expectedCountLow;
}
inline const char* polarityName(bool darkHigh) {
  return darkHigh ? "dark high" : "dark low";
}

// Mirrors one case: the plateau values trade places, `v -> low + high - v`.
inline void mirror(const testdata::Case& c, uint16_t* out) {
  int32_t sum = c.lowPlateau + c.highPlateau;
  for (uint32_t i = 0; i < c.count; ++i) {
    out[i] = static_cast<uint16_t>(sum - c.samples[i]);
  }
}

// Runs one case under one polarity. `darkHigh` selects the mirrored stream.
template <scan::Polarity P>
inline CaseRun run(uint32_t idx, bool darkHigh) {
  const testdata::Case& c = testdata::kCases[idx];

  CaseRun r = {};
  r.index = idx;
  r.darkHigh = darkHigh;
  r.expectedCount = expectedCountAt(idx, darkHigh);

  // The mirror of one case, built once and kept: the on-device self test
  // reports every case in both directions, and a rebuilt mirror would cost
  // one more pass over each stream.
  static uint16_t mirrorBuf[testdata::kMaxSamples];
  const uint16_t* samples = c.samples;
  if (darkHigh) {
    mirror(c, mirrorBuf);
    samples = mirrorBuf;
  }

  scan::State st;
  scan::initState(&st);
  scan::ResultFifo fifo;
  scan::fifoInit(&fifo);

  // Feed in the same chunk quantum the DMA callbacks deliver, carrying the
  // state and the one overlap sample across the boundary.
  uint32_t n = 0;
  uint32_t off = 0;
  while (off < c.count) {
    uint32_t chunk = c.count - off;
    if (chunk > scan::kChunkSamples) chunk = scan::kChunkSamples;
    scan::scan<P>(samples + off, chunk, &st, &fifo);
    off += chunk;
    scan::Result res;
    while (scan::fifoPop(&fifo, &res)) {
      if (n < scan::kFifoSize) {
        r.got[n].present = true;
        r.got[n].ns = res.exposureNs;
        r.got[n].status = scan::statusName(res.status);
      }
      ++n;
    }
  }
  r.gotCount = n;
  return r;
}

}  // namespace scancases

#endif  // SHUTTERCHECK_SCAN_CASES_H