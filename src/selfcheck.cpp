// On-device self test (issue 8). Compiled into ON_DEVICE_TEST builds only.
//
// The firmware normally measures light on PA1. This build keeps the capture
// path running, but it feeds the generated scan vectors to the scan instead of
// the DMA output. Thus the board tests the peripheral configuration, the
// crossing scan and the USB stack with no sensor in front of it. The last
// section (issue 18) measures generated light pulses from TIM2_CH1 (PA0)
// with the real capture path. It needs the test divider on PA0/PA1.
#if defined(ON_DEVICE_TEST) && defined(ARDUINO)
#include "selfcheck.h"

#include <Arduino.h>
#include <stm32f1xx_hal.h>
#include <stm32f1xx_hal_rcc_ex.h>
#include <string.h>

#include "capture.h"
#include "pulsegen.h"
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

// Runs one generated case under one polarity and prints PASS or FAIL. A
// failure prints the wanted and the measured results below the line.
template <scan::Polarity P>
void checkCasePolarity(uint32_t idx, bool darkHigh) {
  const testdata::Expected* expected = scancases::expectedAt(idx, darkHigh);
  const uint32_t expectedCount = scancases::expectedCountAt(idx, darkHigh);
  const scancases::CaseRun r = scancases::run<P>(idx, darkHigh);

  bool ok = r.gotCount == expectedCount;
  for (uint32_t j = 0; ok && j < expectedCount; ++j) {
    ok = r.got[j].present && r.got[j].ns == expected[j].ns &&
         strcmp(r.got[j].status, expected[j].status) == 0;
  }

  Serial.print(ok ? "PASS " : "FAIL ");
  Serial.print("scan case ");
  Serial.print(idx);
  Serial.print(' ');
  Serial.println(scancases::polarityName(darkHigh));
  if (ok) {
    return;
  }
  ++g_failures;

  Serial.print("  want ");
  Serial.print(expectedCount);
  Serial.println(" result(s):");
  for (uint32_t j = 0; j < expectedCount; ++j) {
    Serial.print("    ");
    Serial.print(expected[j].ns);
    Serial.print(' ');
    Serial.println(expected[j].status);
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

// One case, both front-end directions (issue 10).
void checkCase(uint32_t idx) {
  checkCasePolarity<scan::Polarity::kDarkLow>(idx, false);
  checkCasePolarity<scan::Polarity::kDarkHigh>(idx, true);
}

// The generated pulse widths in ms (issue 18): one pulse per duration over
// the 1 ms to 1 s shutter series. Every width is a multiple of the 2 us
// sample period, so the sample grid adds no width error.
constexpr uint32_t kPulseMs[] = {1, 2, 5, 10, 20, 50, 100, 250, 500, 1000};
constexpr uint32_t kMaxScanCycles = 65000;  // 12% margin under 1.024 ms.

constexpr uint32_t kPlateauPrimeMs = 5;

// Scan one discarded pulse to learn the real bright plateau. The first
// threshold after reset is only a dark-side estimate and has RC timing bias.
bool processDarkChunks(uint32_t wanted) {
  uint32_t processed = 0;
  bool foundResult = false;
  while (processed < wanted) {
    if (capture::processNextHalf()) ++processed;
    scan::Result discarded;
    while (capture::nextResult(&discarded)) foundResult = true;
  }
  return foundResult;
}

void primeScan() {
  capture::resetScan();
  processDarkChunks(2);
  pulsegen::arm(kPlateauPrimeMs);

  const uint32_t deadline = millis() + 2 * kPlateauPrimeMs + 50;
  while (pulsegen::isRunning() &&
         static_cast<int32_t>(millis() - deadline) < 0) {
    capture::processNextHalf();
    scan::Result discarded;
    while (capture::nextResult(&discarded)) {
    }
  }
  const bool timerCompleted = !pulsegen::isRunning();
  while (pulsegen::isRunning()) {
    capture::processNextHalf();
    scan::Result discarded;
    while (capture::nextResult(&discarded)) {
    }
  }
  processDarkChunks(2);  // Finish the close edge; discard the priming result.

  const bool ok = timerCompleted;
  Serial.print(ok ? "PASS " : "FAIL ");
  Serial.print("adc pulse conditioning ");
  Serial.print(kPlateauPrimeMs);
  Serial.println(" ms");
  if (!ok) ++g_failures;
}

// One generated light pulse through the test divider, measured by the real
// capture path (issue 18). PASS needs one accepted measurement within 0.5 %
// of the nominal width. The millis() timeout guards the pump loop only: the
// measurement itself stays on the sample clock (design invariant 8).
void checkPulse(uint32_t widthMs) {
  bool unexpected = false;
  // Keep the scan state learned by the conditioning pulse. Do not reset the
  // plateaus between widths; only wait for two dark chunks before each pulse.
  while (pulsegen::isRunning()) {
    capture::processNextHalf();
    scan::Result discarded;
    while (capture::nextResult(&discarded)) unexpected = true;
  }
  if (processDarkChunks(2)) unexpected = true;

  const int64_t nominal = static_cast<int64_t>(widthMs) * 1000000;
  const int64_t tol = nominal / 200;  // 0.5 %.
  pulsegen::arm(widthMs);
  // Wait for a result from a completed timer cycle. Ignore no result silently:
  // record any premature or wrong result as a self-test failure.
  const uint32_t deadline = millis() + 2 * widthMs + 50;
  scan::Result res = {};
  bool haveResult = false;
  bool got = false;
  bool unexpectedWhileTimer = false;
  while (!got && static_cast<int32_t>(millis() - deadline) < 0) {
    capture::processNextHalf();
    scan::Result candidate;
    while (capture::nextResult(&candidate)) {
      // Each generated pulse must produce exactly one result.
      if (haveResult) {
        unexpected = true;
        continue;
      }
      res = candidate;
      haveResult = true;
      const bool timerRunning = pulsegen::isRunning();
      if (timerRunning) unexpectedWhileTimer = true;
      int64_t delta = candidate.exposureNs - nominal;
      if (delta < 0) delta = -delta;
      const bool matches = candidate.status == scan::Status::kOk &&
                           delta <= tol;
      if (timerRunning || !matches) {
        unexpected = true;
        continue;
      }
      got = true;
    }
  }
  const bool ok = got && !unexpected;

  // "PASS adc pulse 1000 ms 999992 ns" per duration (issue 18).
  Serial.print(ok ? "PASS " : "FAIL ");
  Serial.print("adc pulse ");
  Serial.print(widthMs);
  Serial.print(" ms ");
  if (haveResult) {
    Serial.print(res.exposureNs);
    Serial.println(" ns");
  } else {
    Serial.println("no result");
  }
  if (ok) return;
  ++g_failures;

  if (unexpected) Serial.println("  unexpected or early result");
  if (unexpectedWhileTimer) Serial.println("  result while TIM2 active");
  Serial.print("  want ");
  Serial.print(nominal);
  Serial.print(" ns +/-");
  Serial.print(tol);
  Serial.println(" ns");
  Serial.print("  got ");
  if (!haveResult) {
    Serial.println("no result");
    return;
  }
  Serial.print(res.exposureNs);
  Serial.print(" ns ");
  Serial.println(scan::statusName(res.status));
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

  // The ADC pulse section (issue 18): one generated pulse per duration,
  // measured by the real capture path. It needs the test divider on PA0/PA1
  // (see the README). Without the divider every line FAILs, as the issue
  // expects.
  capture::resetTestStats();
  pulsegen::begin();
  primeScan();
  for (uint32_t i = 0; i < sizeof(kPulseMs) / sizeof(kPulseMs[0]); ++i) {
    checkPulse(kPulseMs[i]);
  }
  const uint32_t overwrittenHalves = capture::overwrittenHalves();
  Serial.print("overwritten halves ");
  Serial.println(overwrittenHalves);
  check("DMA half backlog", overwrittenHalves == 0);
  const uint32_t maxScanCycles = capture::maxScanCycles();
  Serial.print("scan max cycles ");
  Serial.println(maxScanCycles);
  check("scan budget", maxScanCycles <= kMaxScanCycles);

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
