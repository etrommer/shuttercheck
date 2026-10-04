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

// True while the capture path is live: ADC1 converts on the TIM3 trigger and
// DMA1_Channel1 transfers into the circular buffer. The on-device self test
// (issue 8) uses it; the measurement path does not.
bool isRunning();

// Pops one finished measurement from the result FIFO. Returns false when the
// FIFO is empty. Must run from loop() only, never from an ISR.
bool nextResult(scan::Result* out);

#if defined(SHUTTERCHECK_DEBUG)
// Raw history ring (issue 16). Debug builds only: a release build compiles
// none of it and pays none of its RAM. Each scanned half lands in the ring
// with the absolute sample index of its first sample, so the ring is a window
// on the sampling grid and not only a list of values.
//
// The ring holds SHUTTERCHECK_DUMP_HALVES halves. Halves per ring x 512
// samples x 2 bytes = 8 KiB at the default of 8, inside the 20 KiB budget.
#ifndef SHUTTERCHECK_DUMP_HALVES
#define SHUTTERCHECK_DUMP_HALVES 8
#endif
constexpr uint32_t kHistoryHalves = SHUTTERCHECK_DUMP_HALVES;
constexpr uint32_t kHistorySamples = kHistoryHalves * kHalfSamples;

// One recorded buffer half: its samples, and the index of the first sample on
// the absolute sampling grid. The index is the true position of the half: it
// counts the halves the DMA finished and the ones the scan missed, so a jump
// from one chunk to the next is a real gap in the recorded signal.
struct HistoryChunk {
  const uint16_t* samples;
  uint64_t startIndex;
};

// Number of recorded chunks in the ring, oldest first.
uint32_t historyCount();

// Chunk `i` of the ring, oldest first. `i` must be below historyCount(). The
// samples stay valid until the next historyResume().
void historyChunk(uint32_t i, HistoryChunk* out);

// Stops recording into the ring while a dump goes out, so the print cannot
// race the DMA. The scan keeps running: only the recording pauses.
void historyFreeze();

// Clears the ring and records again. Call it after a dump: the window after a
// print is not contiguous with the window before it.
void historyResume();

// The scan state, so the debug report can show the plateaus, the span and the
// threshold (issue 12). debug.cpp is the only caller.
const scan::State& state();

// The number of buffer halves that the DMA finished and that the scan never
// saw: the DMA publishes only the newest finished half, so an older finished
// half is gone when a newer one arrives. The DMA callbacks count the halves
// they publish, so the count is exact, also when the loop stalls for more
// than one half. A report that grows says that the print rate or the scan is
// too slow.
uint32_t lostHalves();
#endif

}  // namespace capture

#endif  // SHUTTERCHECK_CAPTURE_H