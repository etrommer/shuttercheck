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
#define DEBUG_CHUNK_INTERVAL 128
#endif
constexpr uint32_t kDebugChunkInterval = DEBUG_CHUNK_INTERVAL;

// Raw sample dump (issue 16). One trigger freezes the window of the last
// SHUTTERCHECK_DUMP_HALVES buffer halves and prints it out, so the raw signal
// behind a measurement is visible. All three intervals are compile-time
// constants: no run-time knob may reappear (design invariant 4).
//
// POSTROLL: halves the ring still records after the trigger, so the dump also
// holds the samples after the event. 2 halves = 4.096 ms.
#ifndef SHUTTERCHECK_DUMP_POSTROLL
#define SHUTTERCHECK_DUMP_POSTROLL 2
#endif
// COOLDOWN: chunks without a new dump after one went out. 32 chunks = 33 ms.
#ifndef SHUTTERCHECK_DUMP_COOLDOWN
#define SHUTTERCHECK_DUMP_COOLDOWN 32
#endif
// IDLE: chunks between two snapshots with no event at all, so the quiet
// baseline prints too. 4096 chunks = 4.2 s. 0 turns the idle snapshot off.
#ifndef SHUTTERCHECK_DUMP_IDLE
#define SHUTTERCHECK_DUMP_IDLE 4096
#endif
constexpr uint32_t kDumpPostRollHalves = SHUTTERCHECK_DUMP_POSTROLL;
constexpr uint32_t kDumpCooldownChunks = SHUTTERCHECK_DUMP_COOLDOWN;
constexpr uint32_t kDumpIdleChunks = SHUTTERCHECK_DUMP_IDLE;

// Samples per printed row. 512 samples per chunk is 32 whole rows, so a row
// never straddles two chunks.
constexpr uint32_t kDumpRowSamples = 16;

// Counts one result that the loop drained from the FIFO, for the ok, stale,
// clip and weak counters and for the `last` fields. A result also arms the
// raw dump (issue 16).
void countResult(const scan::Result& result);

// Prints one report when the counter of scanned chunks reaches the interval.
// `chunkScanned` is the result of capture::processNextHalf(): the counter
// counts scanned chunks, not loop iterations. Runs from loop() only, never
// from an ISR.
void reportDue(bool chunkScanned);

// Prints the raw window of the ring, one trigger at a time (issue 16). A
// result arms it; the ring then records kDumpPostRollHalves halves more, the
// window freezes and the rows go out over the following passes. `chunkScanned`
// is the result of capture::processNextHalf(): the post-roll counts scanned
// chunks, not loop iterations. Runs from loop() only, never from an ISR.
void dumpDue(bool chunkScanned);

}  // namespace debug

#endif  // SHUTTERCHECK_DEBUG_H
#endif  // defined(SHUTTERCHECK_DEBUG)
