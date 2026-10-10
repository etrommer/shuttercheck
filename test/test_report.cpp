// Shutter speed report unit test (native, issue 28).
//
// Every assertion is a literal expectation of the line, or a property of the
// line (the mark search is self-consistent, the line stays short). Nothing
// here reimplements the formatter arithmetic.
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "report.h"

namespace {

// Formats into a static buffer and returns it. One call site per assertion,
// so one buffer is enough.
const char* lineOf(int64_t exposureNs, bool withRawNs = false) {
  static char buf[report::kMaxLineBytes];
  report::format(exposureNs, buf, sizeof(buf), withRawNs);
  return buf;
}

int64_t absOf(int64_t v) { return v < 0 ? -v : v; }

}  // namespace

// The table holds the common speeds, fastest first, and each fractional mark
// carries its own nominal width.
void test_table_covers_the_common_speeds() {
  TEST_ASSERT_EQUAL_UINT(16, report::kMarkCount);
  const report::Mark* marks = report::marks();
  const uint16_t expected[16] = {4000, 2000, 1000, 500, 250, 125, 60, 30,
                                 15,   8,    4,    2,   0,   0,   0,  0};
  for (uint32_t i = 0; i < report::kMarkCount; ++i) {
    TEST_ASSERT_EQUAL_UINT(expected[i], marks[i].denom);
    if (marks[i].denom != 0) {
      TEST_ASSERT_EQUAL_INT(1000000000LL / marks[i].denom, marks[i].ns);
    }
    if (i != 0) TEST_ASSERT_TRUE(marks[i - 1].ns < marks[i].ns);
  }
}

// Each mark, fed back in, prints its own value and no error. The literals are
// the specification; the measured field carries the unit, the mark field does
// not.
void test_each_mark_reports_itself() {
  static const char* const kExpected[16] = {
      "1/4000 s 1/4000 0.0%", "1/2000 s 1/2000 0.0%", "1/1000 s 1/1000 0.0%",
      "1/500 s 1/500 0.0%",   "1/250 s 1/250 0.0%",   "1/125 s 1/125 0.0%",
      "1/60 s 1/60 0.0%",     "1/30 s 1/30 0.0%",     "1/15 s 1/15 0.0%",
      "1/8 s 1/8 0.0%",       "1/4 s 1/4 0.0%",       "0.50 s 1/2 0.0%",
      "1.00 s 1 s 0.0%",      "2.00 s 2 s 0.0%",      "4.00 s 4 s 0.0%",
      "8.00 s 8 s 0.0%"};
  const report::Mark* marks = report::marks();
  for (uint32_t i = 0; i < report::kMarkCount; ++i) {
    TEST_ASSERT_EQUAL_STRING(kExpected[i], lineOf(marks[i].ns));
  }
}

// The fraction form keeps three significant digits: exact integers, one
// decimal, two decimals, and a rounded long value.
void test_fraction_form_keeps_three_significant_digits() {
  TEST_ASSERT_EQUAL_STRING("1/500 s 1/500 0.0%", lineOf(2000000));
  TEST_ASSERT_EQUAL_STRING("1/403 s 1/500 +24.1%", lineOf(2481234));
  TEST_ASSERT_EQUAL_STRING("1/60.6 s 1/60 -1.0%", lineOf(16500000));
  TEST_ASSERT_EQUAL_STRING("1/60 s 1/60 0.0%", lineOf(16666667));
  TEST_ASSERT_EQUAL_STRING("1/1000 s 1/1000 0.0%", lineOf(999999));
  TEST_ASSERT_EQUAL_STRING("1/205000 s 1/4000 -98.1%", lineOf(4867));
  TEST_ASSERT_EQUAL_STRING("1/513 s 1/500 -2.5%", lineOf(1950000));
  TEST_ASSERT_EQUAL_STRING("1/476 s 1/500 +5.0%", lineOf(2100000));
}

// From half a second up the line shows seconds with two decimals.
void test_seconds_form_from_half_a_second() {
  TEST_ASSERT_EQUAL_STRING("0.50 s 1/2 0.0%", lineOf(500000000));
  TEST_ASSERT_EQUAL_STRING("1.00 s 1 s 0.0%", lineOf(999999999));
  TEST_ASSERT_EQUAL_STRING("2.10 s 2 s +5.0%", lineOf(2100000000));
  TEST_ASSERT_EQUAL_STRING("6.00 s 8 s -25.0%", lineOf(6000000000));
  TEST_ASSERT_EQUAL_STRING("8.00 s 8 s 0.0%", lineOf(8000000000));
}

// A tie goes to the faster mark, and the error always names the mark the line
// prints. The search is monotone: a longer width never selects a faster mark.
void test_mark_search_is_consistent_and_monotone() {
  const report::Mark* marks = report::marks();
  int64_t previousIndex = 0;
  int64_t previous = 250000;
  for (int64_t width = 250000; width < 6000000000LL; width += width / 64 + 1) {
    const report::Mark& best = report::nearestMark(width);
    const int64_t bestError = absOf(report::errorPermille(width, best));
    for (uint32_t i = 0; i < report::kMarkCount; ++i) {
      TEST_ASSERT_TRUE(absOf(report::errorPermille(width, marks[i])) >=
                       bestError);
    }
    int64_t index = 0;
    for (uint32_t i = 0; i < report::kMarkCount; ++i) {
      if (&marks[i] == &best) index = i;
    }
    TEST_ASSERT_TRUE(index >= previousIndex);
    TEST_ASSERT_TRUE(width >= previous);
    previousIndex = index;
    previous = width;
  }
}

// The speed of the mark and the printed error can never disagree.
void test_error_agrees_with_the_printed_mark() {
  for (int64_t width = 250000; width < 6000000000LL; width += width / 32 + 1) {
    char expected[24];
    const report::Mark& mark = report::nearestMark(width);
    if (mark.denom != 0) {
      snprintf(expected, sizeof(expected), "1/%u", (unsigned)mark.denom);
    } else {
      snprintf(expected, sizeof(expected), "%lld s",
               (long long)(mark.ns / 1000000000LL));
    }
    const char* line = lineOf(width);
    TEST_ASSERT_TRUE(strstr(line, expected) != NULL);
  }
}

// The debug form keeps the raw width, the release form does not.
void test_raw_width_only_in_the_debug_form() {
  TEST_ASSERT_EQUAL_STRING("1/403 s 1/500 +24.1% ns=2481234",
                           lineOf(2481234, true));
  TEST_ASSERT_TRUE(strstr(lineOf(2481234, false), "ns=") == NULL);
}

// A short buffer truncates and stays terminated; the return value is the byte
// count written.
void test_short_buffer_truncates() {
  char buf[8];
  memset(buf, 'x', sizeof(buf));
  TEST_ASSERT_EQUAL_UINT(0, report::format(2481234, buf, 1, false));
  TEST_ASSERT_EQUAL_CHAR('\0', buf[0]);
  TEST_ASSERT_EQUAL_UINT(7, report::format(2481234, buf, sizeof(buf), false));
  TEST_ASSERT_EQUAL_STRING("1/403 s", buf);
}

// One CDC packet holds 64 bytes. An in-range line stays below that, so the
// write never fills the 128-byte transmit queue.
void test_in_range_line_fits_one_packet() {
  for (int64_t width = 250000; width < 6000000000LL; width += width / 32 + 1) {
    const size_t len = strlen(lineOf(width));
    TEST_ASSERT_TRUE(len < 64);
    const size_t rawLen = strlen(lineOf(width, true));
    TEST_ASSERT_TRUE(rawLen <= report::kMaxLineBytes);
  }
  TEST_ASSERT_TRUE(strlen(lineOf(1)) < report::kMaxLineBytes);
  TEST_ASSERT_TRUE(strlen(lineOf(8000000000LL, true)) <=
                   report::kMaxLineBytes);
}

// The shared entry point is test/test_main.cpp: this function and
// runScanTests() run in one binary.
void runReportTests() {
  RUN_TEST(test_table_covers_the_common_speeds);
  RUN_TEST(test_each_mark_reports_itself);
  RUN_TEST(test_fraction_form_keeps_three_significant_digits);
  RUN_TEST(test_seconds_form_from_half_a_second);
  RUN_TEST(test_mark_search_is_consistent_and_monotone);
  RUN_TEST(test_error_agrees_with_the_printed_mark);
  RUN_TEST(test_raw_width_only_in_the_debug_form);
  RUN_TEST(test_short_buffer_truncates);
  RUN_TEST(test_in_range_line_fits_one_packet);
}
