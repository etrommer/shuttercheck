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

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // Active-low: HIGH = dark.
  Serial.begin(115200);             // Serial == SerialUSB via the CDC flag.

  capture::begin();
  Serial.println("shuttercheck exposure");
}

void loop() {
  // Scan the newest finished buffer half at thread priority, where the USB
  // and DMA interrupts can preempt it (invariant 7).
  capture::processNextHalf();

  scan::Result r;
  while (capture::nextResult(&r)) {
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
}
#endif  // defined(ARDUINO)