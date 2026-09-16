// Shutter edge scan unit test (native).
//
// Feeds the hand-crafted synthetic streams from test/test_data.h through the
// scan in 512-sample chunks (the DMA/callback quantum), drains the result
// FIFO after each chunk and asserts the exact expected result sequence.
// The expected values are literals derived from the spec geometry (crossing
// formula, known thresholds); nothing here replicates the scan algorithm.
#include <unity.h>

#include "scan.h"
#include "test_data.h"

namespace {

void runCase(uint32_t idx) {
  const testdata::Case& c = testdata::kCases[idx];

  scan::State st;
  scan::initState(&st);
  scan::ResultFifo fifo;
  scan::fifoInit(&fifo);

  scan::Result results[scan::kFifoSize];
  uint32_t n = 0;

  // Feed in the same chunk quantum the DMA callbacks deliver, carrying the
  // state and the one overlap sample across the boundary.
  uint32_t off = 0;
  while (off < c.count) {
    uint32_t chunk = c.count - off;
    if (chunk > scan::kChunkSamples) chunk = scan::kChunkSamples;
    scan::scan(c.samples + off, chunk, &st, &fifo);
    off += chunk;
    scan::Result r;
    while (scan::fifoPop(&fifo, &r) && n < scan::kFifoSize) results[n++] = r;
  }

  TEST_ASSERT_EQUAL_UINT32(c.expectedCount, n);
  for (uint32_t j = 0; j < c.expectedCount; ++j) {
    TEST_ASSERT_EQUAL_STRING(c.expected[j].status,
                             scan::statusName(results[j].status));
    TEST_ASSERT_EQUAL_INT64(c.expected[j].ns, results[j].exposureNs);
  }
}

}  // namespace

void test_case0_clean_1_1000() { runCase(0); }
void test_case1_clean_1_4000() { runCase(1); }
void test_case2_crossing_across_chunk() { runCase(2); }
void test_case3_clipped() { runCase(3); }
void test_case4_weak() { runCase(4); }
void test_case5_stale() { runCase(5); }
void test_case6_band_noise() { runCase(6); }
void test_case7_two_pulses() { runCase(7); }

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_case0_clean_1_1000);
  RUN_TEST(test_case1_clean_1_4000);
  RUN_TEST(test_case2_crossing_across_chunk);
  RUN_TEST(test_case3_clipped);
  RUN_TEST(test_case4_weak);
  RUN_TEST(test_case5_stale);
  RUN_TEST(test_case6_band_noise);
  RUN_TEST(test_case7_two_pulses);
  return UNITY_END();
}