// Capture path: TIM3 (TRGO at 500 kS/s) drives ADC1; DMA1_Channel1 lands the
// samples in a circular buffer of two halves. The crossing scan runs in the
// DMA half/transfer-complete callbacks; finished measurements are published
// through a result FIFO drained by loop().
#ifndef SHUTTERCHECK_CAPTURE_H
#define SHUTTERCHECK_CAPTURE_H

#include <stdint.h>

#include "scan.h"

namespace capture {

// Samples per buffer half. The scan of one half must stay well inside its
// period: 512 samples x 2.000 us = ~1.024 ms (design invariant 7).
constexpr uint32_t kHalfSamples = 512;
constexpr uint32_t kSamplePeriodNs = 2000;

// Configures and starts TIM3 + ADC1 + DMA. Runs the ADC calibration first;
// no conversion happens before it ends.
void begin();

// Pops one finished measurement from the result FIFO. Returns false when the
// FIFO is empty. Must run from loop() only, never from an ISR.
bool nextResult(scan::Result* out);

}  // namespace capture

#endif  // SHUTTERCHECK_CAPTURE_H