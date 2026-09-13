// shuttercheck — ADC driver: periodic readings over USB (issue 3).
//
// setup() starts the capture path (calibrated first) and loop() reports at
// 10 Hz: one line per sample of the newest finished buffer half, then one
// short LED flash on PB12. No measurement arithmetic happens here.
#include <Arduino.h>

#include "capture.h"

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // Active-low: HIGH = dark.
  Serial.begin(115200);             // Serial == SerialUSB via the CDC flag.

  capture::begin();
  Serial.println("shuttercheck adc stream");
}

void loop() {
  static uint32_t nextReportMs = 0;
  uint32_t now = millis();
  if (now < nextReportMs) {
    return;
  }
  nextReportMs = now + 100;  // Ten reports per second.

  const volatile uint16_t* samples = nullptr;
  if (!capture::newestHalf(samples)) {
    return;  // No DMA data yet; print nothing.
  }
  for (uint32_t i = 0; i < capture::kHalfSamples; i++) {
    Serial.print("adc ");
    Serial.println(samples[i], DEC);
  }

  // One short flash per report, active-low PB12.
  digitalWrite(LED_BUILTIN, LOW);
  delay(2);
  digitalWrite(LED_BUILTIN, HIGH);
}
