// On-device self test (issue 8). Compiled into ON_DEVICE_TEST builds only.
//
// The firmware normally measures light on PA1. This build keeps the capture
// path running, but it feeds the generated scan vectors to the scan instead of
// the DMA output. Thus the board tests the peripheral configuration, the
// crossing scan and the USB stack with no sensor in front of it.
#if defined(ON_DEVICE_TEST) && defined(ARDUINO)
#include "selfcheck.h"

#include <Arduino.h>
#include <stm32f1xx_hal.h>
#include <stm32f1xx_hal_rcc_ex.h>
#include <string.h>

#include "capture.h"
#include "scan_cases.h"

namespace selfcheck {
namespace {

uint32_t g_failures = 0;

// Prints "PASS <name>" or "FAIL <name>" and counts the failure.
void check(const char* name, bool ok) {
  Serial.print(ok ? "PASS " : "FAIL ");
  Serial.println(name);
  if (!ok) {
    ++g_failures;
  }
}

// Prints one clock check with the measured frequency in MHz.
void checkClock(const char* name, uint32_t gotHz, uint32_t wantHz) {
  const bool ok = gotHz == wantHz;
  Serial.print(ok ? "PASS " : "FAIL ");
  Serial.print(name);
  Serial.print(' ');
  Serial.print(gotHz / 1000000UL);
  Serial.print(" MHz");
  if (!ok) {
    Serial.print(", want ");
    Serial.print(wantHz / 1000000UL);
    Serial.print(" MHz");
    ++g_failures;
  }
  Serial.println();
}

// Runs one generated case and prints PASS or FAIL. A failure prints the
// wanted and the measured results below the line.
void checkCase(uint32_t idx) {
  const testdata::Case& c = scancases::caseAt(idx);
  const scancases::CaseRun r = scancases::run(idx);

  bool ok = r.gotCount == c.expectedCount;
  for (uint32_t j = 0; ok && j < c.expectedCount; ++j) {
    ok = r.got[j].present && r.got[j].ns == c.expected[j].ns &&
         strcmp(r.got[j].status, c.expected[j].status) == 0;
  }

  Serial.print(ok ? "PASS " : "FAIL ");
  Serial.print("scan case ");
  Serial.println(idx);
  if (ok) {
    return;
  }
  ++g_failures;

  Serial.print("  want ");
  Serial.print(c.expectedCount);
  Serial.println(" result(s):");
  for (uint32_t j = 0; j < c.expectedCount; ++j) {
    Serial.print("    ");
    Serial.print(c.expected[j].ns);
    Serial.print(' ');
    Serial.println(c.expected[j].status);
  }

  Serial.print("  got ");
  Serial.print(r.gotCount);
  Serial.println(" result(s):");
  const uint32_t shown = r.gotCount < scan::kFifoSize ? r.gotCount
                                                      : scan::kFifoSize;
  for (uint32_t j = 0; j < shown; ++j) {
    Serial.print("    ");
    Serial.print(r.got[j].ns);
    Serial.print(' ');
    Serial.println(r.got[j].status);
  }
}

// How long the report waits for a host to open the CDC port. USBSerial
// reports the DTR state and delays 10 ms for each read of it. Keep this
// shorter than the read timeout of scripts/on_device_test.py.
constexpr uint32_t kHostWaitPolls = 1000;  // ~10 s.

}  // namespace

uint32_t run() {
  // The report must not go out before anyone can read it.
  for (uint32_t i = 0; i < kHostWaitPolls && !Serial; ++i) {
  }

  checkClock("sysclk", HAL_RCC_GetSysClockFreq(), 72000000UL);
  checkClock("hclk", HAL_RCC_GetHCLKFreq(), 72000000UL);
  checkClock("pclk1", HAL_RCC_GetPCLK1Freq(), 36000000UL);
  checkClock("pclk2", HAL_RCC_GetPCLK2Freq(), 72000000UL);
  checkClock("adcclk", HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_ADC),
             12000000UL);
  checkClock("usbclk", HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_USB),
             48000000UL);
  check("capture running", capture::isRunning());
  check("usb host connected", static_cast<bool>(Serial));

  for (uint32_t i = 0; i < scancases::caseCount(); ++i) {
    checkCase(i);
  }

  if (g_failures == 0) {
    Serial.println("ALL TESTS PASSED");
  } else {
    Serial.print("FAILURES: ");
    Serial.println(g_failures);
  }
  return g_failures;
}

}  // namespace selfcheck
#endif  // defined(ON_DEVICE_TEST) && defined(ARDUINO)
