// Shutter edge scan unit test (native).
//
// Feeds the hand-crafted synthetic streams from test/test_data.h through the
// scan and asserts the exact expected result sequence. The feeder is
// test/scan_cases.h, shared with the on-device self test (issue 8), so both
// reporters see the same results. The expected values are literals derived
// from the spec geometry (crossing formula, known thresholds); nothing here
// replicates the scan algorithm.
#include <unity.h>

#include "scan_cases.h"

namespace {

void runCase(uint32_t idx) {
  const testdata::Case& c = scancases::caseAt(idx);
  const scancases::CaseRun r = scancases::run(idx);

  TEST_ASSERT_EQUAL_UINT32(c.expectedCount, r.gotCount);
  for (uint32_t j = 0; j < c.expectedCount; ++j) {
    TEST_ASSERT_TRUE(r.got[j].present);
    TEST_ASSERT_EQUAL_STRING(c.expected[j].status, r.got[j].status);
    TEST_ASSERT_EQUAL_INT64(c.expected[j].ns, r.got[j].ns);
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
