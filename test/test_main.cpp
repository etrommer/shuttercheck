// Native unit test entry point (issues 5 and 28).
//
// PlatformIO compiles every file in test/ into one binary, so the two test
// groups share one main: test_scan.cpp covers the scan, test_report.cpp
// covers the report format.
#include <unity.h>

void runScanTests();
void runReportTests();

int main() {
  UNITY_BEGIN();
  runScanTests();
  runReportTests();
  return UNITY_END();
}
