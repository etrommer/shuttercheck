// Capture path: TIM3 (TRGO at 500 kS/s) drives ADC1; DMA1_Channel1 lands the
// samples in a circular buffer of two halves. The DMA callbacks only flag the
// newest finished half (an O(1) handshake); the crossing scan runs in loop()
// on the finished half at thread priority, so the USB and DMA interrupts can
// preempt it. Finished measurements go through a result FIFO drained by
// loop().
#ifndef SHUTTERCHECK_CAPTURE_H
#define SHUTTERCHECK_CAPTURE_H

#include <stdint.h>

#include "scan.h"

namespace capture {

// Samples per buffer half = one scanned chunk. The scan of one half must stay
// comfortably inside its period: 512 samples x 2.000 us = ~1.024 ms (design
// invariant 7).
constexpr uint32_t kHalfSamples = 512;
constexpr uint32_t kSamplePeriodNs = 2000;

// Configures and starts TIM3 + ADC1 + DMA. Runs the ADC calibration first;
// no conversion happens before it ends.
void begin();

// Scans the newest finished buffer half (if any) and returns true. Drives
// the crossing scan at thread priority; call it from loop() before draining
// the result FIFO.
bool processNextHalf();

// Pops one finished measurement from the result FIFO. Returns false when the
// FIFO is empty. Must run from loop() only, never from an ISR.
bool nextResult(scan::Result* out);

}  // namespace capture

#endif  // SHUTTERCHECK_CAPTURE_H