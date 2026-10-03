// shuttercheck — shutter edge scan and exposure reporting (issue 5).
//
// setup() starts the capture path (calibrated first). loop() runs the
// crossing scan on each finished buffer half at thread priority, then drains
// the result FIFO: an accepted pulse prints the exposure in nanoseconds, a
// rejection prints a zero value, and an accepted sample flashes the LED
// once. No measurement arithmetic happens here (design invariant 7: report
// from loop() only).
#if defined(ARDUINO)
#include <Arduino.h>

#include "capture.h"
#include "scan.h"
#if defined(SHUTTERCHECK_DEBUG)
#include "debug.h"
#endif
#if defined(ON_DEVICE_TEST)
#include "selfcheck.h"
#endif

// Failures of the on-device self test (issue 8). The LED blinks for them.
#if defined(ON_DEVICE_TEST)
namespace {
uint32_t g_selfCheckFailures = 0;
}  // namespace
#endif

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // Active-low: HIGH = dark.
  Serial.begin(115200);             // Serial == SerialUSB via the CDC flag.

  capture::begin();
#if defined(ON_DEVICE_TEST)
  // The self test replaces the measurement: it feeds the generated scan
  // vectors instead of the DMA output and prints its report through this
  // same CDC port (issue 8).
  g_selfCheckFailures = selfcheck::run();
#else
  Serial.println("shuttercheck exposure");
#endif
}

void loop() {
#if defined(ON_DEVICE_TEST)
  // The self test ran once in setup(). Blink for failures; stay dark when
  // everything passed.
  if (g_selfCheckFailures != 0) {
    digitalWrite(LED_BUILTIN, LOW);
    delay(125);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(125);
  }
#else
  // Scan the newest finished buffer half at thread priority, where the USB
  // and DMA interrupts can preempt it (invariant 7). Only the debug build
  // needs to know whether a chunk was scanned.
#if defined(SHUTTERCHECK_DEBUG)
  const bool chunkScanned = capture::processNextHalf();
#else
  capture::processNextHalf();
#endif
  scan::Result r;
  while (capture::nextResult(&r)) {
#if defined(SHUTTERCHECK_DEBUG)
    debug::countResult(r);
#endif
    if (r.status == scan::Status::kOk) {
      // "<exposure ns> ok"
      Serial.print(r.exposureNs);
      Serial.print(' ');
      Serial.println(scan::statusName(r.status));
      // One short flash per accepted sample; nothing on rejection.
      digitalWrite(LED_BUILTIN, LOW);
      delay(2);
      digitalWrite(LED_BUILTIN, HIGH);
    } else {
      // "0 <status>" for the rejection paths.
      Serial.print("0 ");
      Serial.println(scan::statusName(r.status));
    }
  }
#if defined(SHUTTERCHECK_DEBUG)
  // Report the state of the capture path and the scan after the drain, so the
  // `last` fields show the newest result. The debug output is extra output:
  // it does not change the exposure, the status or the LED (issue 12).
  debug::reportDue(chunkScanned);
#endif
#endif  // defined(ON_DEVICE_TEST)
}
#endif  // defined(ARDUINO)