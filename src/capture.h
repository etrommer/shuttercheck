// Capture path: TIM3 (TRGO at 500 kS/s) drives ADC1; DMA1_Channel1 lands the
// samples in a circular buffer of two halves. The crossing scan (a later
// issue) runs in the DMA half/transfer-complete callbacks.
#ifndef SHUTTERCHECK_CAPTURE_H
#define SHUTTERCHECK_CAPTURE_H

#include <stdint.h>

namespace capture {

// Samples per buffer half. The scan of one half must stay well inside its
// period: 512 samples x 2.000 us = ~1.024 ms (design invariant 7).
constexpr uint32_t kHalfSamples = 512;
constexpr uint32_t kSamplePeriodNs = 2000;

// Configures and starts TIM3 + ADC1 + DMA. Runs the ADC calibration first;
// no conversion happens before it ends.
void begin();

// Snapshot of the newest finished buffer half. Returns false when no DMA
// data has arrived yet. The pointer stays valid and stable: the DMA is
// filling the other half while you read this one.
bool newestHalf(const volatile uint16_t*& out);

}  // namespace capture

#endif  // SHUTTERCHECK_CAPTURE_H
