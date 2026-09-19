// Shared scan case runner (issue 8).
//
// Feeds one generated case from test/test_data.h through the scan in the
// 512-sample DMA chunk quantum and records what came out. It is free of Unity
// and of the HAL, so the native unit test and the on-device self test share
// one feeder and one set of expectations: only the reporter differs.
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

// What one case produced. Compare `got` against
// testdata::kCases[index].expected.
struct CaseRun {
  uint32_t index;
  uint32_t expectedCount;
  uint32_t gotCount;
  Observed got[scan::kFifoSize];
};

inline uint32_t caseCount() { return testdata::kNumCases; }
inline const testdata::Case& caseAt(uint32_t idx) { return testdata::kCases[idx]; }

inline CaseRun run(uint32_t idx) {
  const testdata::Case& c = testdata::kCases[idx];

  CaseRun r = {};
  r.index = idx;
  r.expectedCount = c.expectedCount;

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
    scan::scan(c.samples + off, chunk, &st, &fifo);
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
