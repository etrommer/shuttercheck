// shuttercheck — Hello World build check.
//
// This sketch only proves the build chain. The capture path (TIM3 + ADC1 +
// DMA1_Channel1) starts in a later issue, so `delay()` is safe here.

#include <Arduino.h>

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);  // Serial == SerialUSB via the CDC build flag.
  Serial.println("shuttercheck hello");
}

void loop() {
  digitalWrite(LED_BUILTIN, LOW);  // PB12 = Blackpill LED, active-low.
  delay(500);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  Serial.println("shuttercheck hello");
}