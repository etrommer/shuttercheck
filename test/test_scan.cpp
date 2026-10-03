// Shutter edge scan unit test (native).
//
// Feeds the hand-crafted synthetic streams from test/test_data.h through the
// scan and asserts the exact expected result sequence. The feeder is
// test/scan_cases.h, shared with the on-device self test (issue 8), so both
// reporters see the same results. The expected values are literals derived
// from the spec geometry (crossing formula, known thresholds); nothing here
// replicates the scan algorithm.
//
// Every case runs under both signal polarities (issue 10): the direct-coupled
// front end on the generated stream, the cascode front end on its mirror.
#include <unity.h>

#include "scan_cases.h"

namespace {

void runCasePolarity(uint32_t idx, bool darkHigh) {
  const testdata::Expected* expected =
      scancases::expectedAt(idx, darkHigh);
  const uint32_t expectedCount = scancases::expectedCountAt(idx, darkHigh);
  const scancases::CaseRun r = darkHigh
      ? scancases::run<scan::Polarity::kDarkHigh>(idx, true)
      : scancases::run<scan::Polarity::kDarkLow>(idx, false);

  TEST_ASSERT_EQUAL_UINT32(expectedCount, r.gotCount);
  for (uint32_t j = 0; j < expectedCount; ++j) {
    TEST_ASSERT_TRUE(r.got[j].present);
    TEST_ASSERT_EQUAL_STRING(expected[j].status, r.got[j].status);
    TEST_ASSERT_EQUAL_INT64(expected[j].ns, r.got[j].ns);
  }
}

void runCase(uint32_t idx) {
  runCasePolarity(idx, false);
  runCasePolarity(idx, true);
}

}  // namespace

#if defined(SHUTTERCHECK_DEBUG)
// The debug extremes of a chunk (issue 12). The extremes must cover the
// whole chunk, also the runs that skipQuietRun() skips.
constexpr uint16_t kFlatSample = 1234;   // One flat plateau, case B.
constexpr uint16_t kDarkLevel = 4000;    // Cascode front end: dark is high.
constexpr uint16_t kBrightLevel = 2000;  // Lit, near V_bias.

// Scans one chunk of `count` samples and returns the state.
scan::State scanChunk(scan::State state, const uint16_t* samples,
                      uint32_t count) {
  scan::ResultFifo fifo;
  scan::fifoInit(&fifo);
  scan::scan<scan::Polarity::kDarkHigh>(samples, count, &state, &fifo);
  return state;
}

// A flat chunk: every sample after the first is skipped by the quiet-run
// fast path, so the extremes come only from the skipped run.
void test_debug_extremes_of_a_skipped_flat_chunk() {
  static uint16_t samples[scan::kChunkSamples];
  for (uint32_t i = 0; i < scan::kChunkSamples; ++i) {
    samples[i] = kFlatSample;
  }
  scan::State state;
  scan::initState(&state);
  const scan::State out = scanChunk(state, samples, scan::kChunkSamples);
  TEST_ASSERT_EQUAL_INT32(kFlatSample, out.debugMin);
  TEST_ASSERT_EQUAL_INT32(kFlatSample, out.debugMax);
}

// A chunk with a step: the plateau run is skipped, the lit run goes through
// the full path. The extremes must span both levels.
void test_debug_extremes_of_a_chunk_with_a_step() {
  static uint16_t samples[scan::kChunkSamples];
  const uint32_t half = scan::kChunkSamples / 2;
  for (uint32_t i = 0; i < scan::kChunkSamples; ++i) {
    samples[i] = i < half ? kDarkLevel : kBrightLevel;
  }
  scan::State state;
  scan::initState(&state);
  const scan::State out = scanChunk(state, samples, scan::kChunkSamples);
  TEST_ASSERT_EQUAL_INT32(kBrightLevel, out.debugMin);
  TEST_ASSERT_EQUAL_INT32(kDarkLevel, out.debugMax);
}

// The extremes are per chunk: the next chunk resets them, so a chunk at
// another level must not carry the levels of the chunk before it.
void test_debug_extremes_reset_per_chunk() {
  static uint16_t first[scan::kChunkSamples];
  static uint16_t second[scan::kChunkSamples];
  for (uint32_t i = 0; i < scan::kChunkSamples; ++i) {
    first[i] = i < scan::kChunkSamples / 2 ? kDarkLevel : kBrightLevel;
    second[i] = kFlatSample;
  }
  scan::State state;
  scan::initState(&state);
  state = scanChunk(state, first, scan::kChunkSamples);
  TEST_ASSERT_EQUAL_INT32(kBrightLevel, state.debugMin);
  const scan::State out = scanChunk(state, second, scan::kChunkSamples);
  TEST_ASSERT_EQUAL_INT32(kFlatSample, out.debugMin);
  TEST_ASSERT_EQUAL_INT32(kFlatSample, out.debugMax);
}

// A chunk whose lowest sample reaches the fast path only: after the seed and
// one full-path sample, prev lies strictly inside the hysteresis band, so
// skipQuietRun() skips the rest of the chunk as one case A run. Without the
// extreme update on that path, the report would miss the lowest level.
void test_debug_extremes_of_a_skipped_in_band_chunk() {
  static uint16_t samples[scan::kChunkSamples];
  samples[0] = 2900;  // Seeds the dark plateau and the 16 LSB bright spread.
  samples[1] = 2895;  // Inside the band, above the threshold: no crossing.
  for (uint32_t i = 2; i < scan::kChunkSamples; ++i) {
    // Also inside the band, so every one of these samples is skipped.
    samples[i] = (i % 2) ? 2889 : 2895;
  }
  scan::State state;
  scan::initState(&state);
  const scan::State out = scanChunk(state, samples, scan::kChunkSamples);
  TEST_ASSERT_EQUAL_INT32(2889, out.debugMin);
  TEST_ASSERT_EQUAL_INT32(2900, out.debugMax);
}
#endif  // defined(SHUTTERCHECK_DEBUG)

void test_case0_clean_1_1000() { runCase(0); }
void test_case1_clean_1_4000() { runCase(1); }
void test_case2_crossing_across_chunk() { runCase(2); }
void test_case3_clipped() { runCase(3); }
void test_case4_weak() { runCase(4); }
void test_case5_stale() { runCase(5); }
void test_case6_band_noise() { runCase(6); }
void test_case7_two_pulses() { runCase(7); }
void test_case8_cascode_dark_at_rail() { runCase(8); }
void test_case9_cascode_operating_point() { runCase(9); }

int main() {
  UNITY_BEGIN();
#if defined(SHUTTERCHECK_DEBUG)
  RUN_TEST(test_debug_extremes_of_a_skipped_flat_chunk);
  RUN_TEST(test_debug_extremes_of_a_chunk_with_a_step);
  RUN_TEST(test_debug_extremes_reset_per_chunk);
  RUN_TEST(test_debug_extremes_of_a_skipped_in_band_chunk);
#endif
  RUN_TEST(test_case0_clean_1_1000);
  RUN_TEST(test_case1_clean_1_4000);
  RUN_TEST(test_case2_crossing_across_chunk);
  RUN_TEST(test_case3_clipped);
  RUN_TEST(test_case4_weak);
  RUN_TEST(test_case5_stale);
  RUN_TEST(test_case6_band_noise);
  RUN_TEST(test_case7_two_pulses);
  RUN_TEST(test_case8_cascode_dark_at_rail);
  RUN_TEST(test_case9_cascode_operating_point);
  return UNITY_END();
}