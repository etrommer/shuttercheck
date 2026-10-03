// Debug mode (issue 12): print the capture and the scan state to the console.
//
// A debug build prints one statistics line per report on the same USB CDC
// port as the exposure report. The build is off by default: the whole module
// is inside `#if defined(SHUTTERCHECK_DEBUG)`, and nothing else in the build
// includes this header, so a release build does not compile the module.
//
// The module reads the path, not the registers: it asks capture for the scan
// state and for the halves it lost, and it owns only the counters of this
// file. The debug output never changes the measurement: the scan, the
// threshold rule and the LED stay as they are.
#if defined(SHUTTERCHECK_DEBUG)
#ifndef SHUTTERCHECK_DEBUG_H
#define SHUTTERCHECK_DEBUG_H

#include <stdint.h>

#include "scan.h"

namespace debug {

// Print one report every kDebugChunkInterval scanned chunks. 16 gives about
// 61 reports per second at 500 kS/s, which fits the 115200 baud port. Define
// DEBUG_CHUNK_INTERVAL on the command line to change it. The interval is a
// compile-time constant: no run-time knob may reappear (design invariant 4).
#ifndef DEBUG_CHUNK_INTERVAL
#define DEBUG_CHUNK_INTERVAL 16
#endif
constexpr uint32_t kDebugChunkInterval = DEBUG_CHUNK_INTERVAL;

// Counts one result that the loop drained from the FIFO, for the ok, stale,
// clip and weak counters and for the `last` fields.
void countResult(const scan::Result& result);

// Prints one report when the counter of scanned chunks reaches the interval.
// `chunkScanned` is the result of capture::processNextHalf(): the counter
// counts scanned chunks, not loop iterations. Runs from loop() only, never
// from an ISR.
void reportDue(bool chunkScanned);

}  // namespace debug

#endif  // SHUTTERCHECK_DEBUG_H
#endif  // defined(SHUTTERCHECK_DEBUG)