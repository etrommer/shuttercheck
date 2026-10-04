// Debug mode (issue 12): the capture and the scan state on the console.
// ARDUINO guards the file too: the native unit-test build compiles every
// source file of src/, and the module talks to Arduino and to the capture
// path. The debug build is firmware-only.
//
// The whole file is inside `#if defined(SHUTTERCHECK_DEBUG)`: a release build
// does not compile it at all. The module reads the path, not the registers:
// it asks capture for the scan state and for the halves it lost, and it owns
#if defined(ARDUINO) && defined(SHUTTERCHECK_DEBUG)
#include "debug.h"

#include <stdio.h>

#include <Arduino.h>

#include "capture.h"

namespace debug {
namespace {

// One statistics line, one buffer, one write: every byte that waits in the
// USB queue blocks the loop, and a blocked loop loses buffer halves.
constexpr size_t kLineBytes = 192;

// The counters this module owns.
uint32_t g_chunks = 0;         // Scanned chunks since the start.
int32_t g_runMin = 0x7FFFFFFF;  // Lowest raw sample since the start.
int32_t g_runMax = -0x7FFFFFFF; // Highest raw sample since the start.
uint32_t g_ok = 0;
uint32_t g_stale = 0;
uint32_t g_clip = 0;
uint32_t g_weak = 0;
bool g_haveResult = false;
scan::Status g_lastStatus = scan::Status::kOk;
int64_t g_lastNs = 0;
bool g_legendPrinted = false;

// The plateau-to-plateau span, always positive and independent of the
// polarity, so the report works with either front end.
int32_t spanOf(const scan::State& s) {
  return s.dark > s.bright ? s.dark - s.bright : s.bright - s.dark;
}

// The threshold, the midpoint of the two plateaus: the rule of the scan.
int32_t thrOf(const scan::State& s) { return (s.dark + s.bright) / 2; }

// The hysteresis half band, as the scan computes it: max(span/16, 8). The
// report needs the full width, so the two values are named apart on purpose:
// currentBand() in scan.cpp returns this half, not the full width.
int32_t hysteresisHalf(int32_t span) {
  const int32_t half = span / 16;
  return half < 8 ? 8 : half;
}

// Writes the decimal form of `value` at `out` and returns the byte count.
// The printf of this toolchain has no long long width: %lld prints "ld", so
// the exposure is formatted here. The call gives a buffer of 24 bytes.
constexpr size_t kInt64Bytes = 24;
size_t appendInt64(char* out, int64_t value) {
  char digits[20];  // 19 digits and a sign are enough for 64 bits.
  size_t n = 0;
  // Negation on the unsigned value: -value is undefined for the smallest
  // negative value.
  uint64_t magnitude =
      value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
  do {
    digits[n++] = static_cast<char>('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0);
  if (value < 0) digits[n++] = '-';
  for (size_t i = 0; i < n; ++i) out[i] = digits[n - 1 - i];
  return n;
}

void printLegend() {
  Serial.print("# shuttercheck debug: one report per ");
  Serial.print(kDebugChunkInterval);
  Serial.println(" scanned chunks; levels in LSB");
}

// The running extremes cover every scanned chunk, not only the reported
// ones: a level that moves for one chunk and returns would stay invisible
// otherwise. Two loads and two compares per chunk, no printing.
void trackRunExtremes() {
  const scan::State& s = capture::state();
  if (s.debugMin < g_runMin) g_runMin = s.debugMin;
  if (s.debugMax > g_runMax) g_runMax = s.debugMax;
}

void printReport() {
  const scan::State& s = capture::state();
  const int32_t span = spanOf(s);
  const int64_t ns = g_haveResult ? g_lastNs : 0;
  const char* last = g_haveResult ? scan::statusName(g_lastStatus) : "none";

  static char line[kLineBytes];
  const int n = snprintf(line, sizeof(line),
                         "#%u min=%d max=%d lo=%d hi=%d dark=%d bright=%d "
                         "span=%d thr=%d band=%d rail=%u pulse=%u lost=%u "
                         "ok=%u stale=%u clip=%u weak=%u last=%s ns=",
                         static_cast<unsigned>(g_chunks), s.debugMin,
                         s.debugMax, g_runMin, g_runMax, s.dark, s.bright,
                         span, thrOf(s), 2 * hysteresisHalf(span),
                         static_cast<unsigned>(s.hitRail),
                         static_cast<unsigned>(s.inPulse),
                         capture::lostHalves(), g_ok, g_stale, g_clip, g_weak,
                         last);
  if (n <= 0 || static_cast<size_t>(n) + kInt64Bytes + 2 > sizeof(line)) {
    return;  // Never write past the buffer.
  }
  size_t total = static_cast<size_t>(n);
  total += appendInt64(line + total, ns);
  line[total++] = '\n';
  Serial.write(reinterpret_cast<const uint8_t*>(line), total);
}


// Raw dump (issue 16). One trigger freezes the recorded window and prints it
// out over the following loop passes: the print cannot race the ring, and the
// loop keeps scanning between the rows.
enum class DumpPhase : uint8_t {
  kIdle,      // Nothing in flight.
  kArmed,     // A trigger arrived; the post-roll is running.
  kPrinting,  // The window is frozen and goes out row by row.
};

DumpPhase g_dumpPhase = DumpPhase::kIdle;
uint32_t g_postRoll = 0;    // Halves still to record after the trigger.
uint32_t g_cooldown = 0;    // Chunks that may not trigger a new dump.
uint32_t g_dumpChunks = 0;  // Chunks since start, for the idle snapshot.
uint32_t g_rowsTotal = 0;   // Rows of the window in flight.
uint32_t g_rowCursor = 0;   // Next row to print.

// One row: "! <index>: <16 samples>\n". The index takes 20 bytes at most and
// one sample 6, so 128 bytes are enough.
constexpr size_t kDumpRowBytes = 128;
char g_row[kDumpRowBytes];

// Rows per pass. A window goes out in passes, not in one long write: four
// rows are about 360 bytes, inside the CDC queue of this build, and the scan
// runs again between two passes.
constexpr uint32_t kDumpRowsPerPass = 4;

// Arms a dump. A dump that is already in flight, or a dump that went out less
// than the cooldown ago, keeps the trigger from re-arming: one exposure gives
// one window, not one window per measurement.
void armDump() {
  if (g_dumpPhase != DumpPhase::kIdle || g_cooldown != 0) return;
  g_dumpPhase = DumpPhase::kArmed;
  g_postRoll = kDumpPostRollHalves;
}

// One dump line is one write on the wire. Serial.write() writes what fits in
// the CDC queue and returns that count, so a longer line can go out in parts
// and another print can land between them: a row of samples split by a
// measurement line is no longer a row. Write the rest here, and give up after
// a bounded wait, so a host that reads nothing cannot wedge the loop.
void writeLine(const char* line, size_t n) {
  constexpr uint32_t kStallLimit = 64;  // 64 x 50 us = 3.2 ms, one USB frame
                                        // drains a full packet in that time.
  size_t done = 0;
  uint32_t stalls = 0;
  while (done < n) {
    const size_t wrote =
        Serial.write(reinterpret_cast<const uint8_t*>(line + done), n - done);
    if (wrote == 0) {
      if (++stalls >= kStallLimit) return;
      delayMicroseconds(50);
      continue;
    }
    done += wrote;
  }
}

// One row of raw samples: "! <index>: <samples>\n". The index is the position
// of the first sample on the absolute sampling grid, so the reader gets the
// time of every row and sees every gap.
void printRow(uint64_t index, const uint16_t* samples) {
  size_t n = 0;
  g_row[n++] = '!';
  g_row[n++] = ' ';
  n += appendInt64(g_row + n, static_cast<int64_t>(index));
  g_row[n++] = ':';
  for (uint32_t i = 0; i < kDumpRowSamples; ++i) {
    g_row[n++] = ' ';
    n += appendInt64(g_row + n, static_cast<int64_t>(samples[i]));
  }
  g_row[n++] = '\n';
  writeLine(g_row, n);
}

// "!gap <n> samples": the DMA finished halves that the scan never saw, so
// those samples are missing between the last chunk and this one. The indices
// of the rows are still the true ones, so the gap is also the real time that
// the loop lost.
void printGap(uint64_t missing) {
  size_t n = 0;
  g_row[n++] = '!';
  g_row[n++] = 'g';
  g_row[n++] = 'a';
  g_row[n++] = 'p';
  g_row[n++] = ' ';
  n += appendInt64(g_row + n, static_cast<int64_t>(missing));
  const char tail[] = " samples\n";
  for (size_t i = 0; i < sizeof(tail) - 1; ++i) g_row[n++] = tail[i];
  writeLine(g_row, n);
}

// Freezes the window and prints its header with the threshold of the moment,
// so the raw samples and the rule that judged them stand next to each other.
void startDump() {
  capture::historyFreeze();
  g_dumpPhase = DumpPhase::kPrinting;
  const uint32_t chunks = capture::historyCount();
  const uint32_t rowsPerChunk = capture::kHalfSamples / kDumpRowSamples;
  g_rowsTotal = chunks * rowsPerChunk;
  g_rowCursor = 0;

  uint64_t base = 0;
  if (chunks > 0) {
    capture::HistoryChunk first;
    capture::historyChunk(0, &first);
    base = first.startIndex;
  }
  const scan::State& s = capture::state();
  const int32_t span = spanOf(s);
  char head[96];
  int m = snprintf(head, sizeof(head), "!dump samples=%u rows=%u base=",
                   static_cast<unsigned>(chunks * capture::kHalfSamples),
                   static_cast<unsigned>(g_rowsTotal));
  if (m <= 0 || static_cast<size_t>(m) + kInt64Bytes > sizeof(head)) return;
  m += static_cast<int>(appendInt64(head + m, static_cast<int64_t>(base)));
  m += snprintf(head + m, sizeof(head) - static_cast<size_t>(m),
                " thr=%d band=%d\n", static_cast<int>(thrOf(s)),
                static_cast<int>(2 * hysteresisHalf(span)));
  if (m <= 0) return;
  writeLine(head, static_cast<size_t>(m));
}

// Prints the rows of this pass. Returns true when the window is out.
bool printDumpRows() {
  const uint32_t rowsPerChunk = capture::kHalfSamples / kDumpRowSamples;
  for (uint32_t pass = 0; pass < kDumpRowsPerPass && g_rowCursor < g_rowsTotal;
       ++pass) {
    const uint32_t chunk = g_rowCursor / rowsPerChunk;
    const uint32_t row = g_rowCursor % rowsPerChunk;
    capture::HistoryChunk ch;
    capture::historyChunk(chunk, &ch);
    // The first row of a chunk: the index says whether the chunk follows the
    // previous one on the grid or whether halves are missing between them.
    if (row == 0 && chunk > 0) {
      capture::HistoryChunk prev;
      capture::historyChunk(chunk - 1, &prev);
      if (ch.startIndex != prev.startIndex + capture::kHalfSamples) {
        printGap(ch.startIndex - prev.startIndex - capture::kHalfSamples);
      }
    }
    printRow(ch.startIndex + static_cast<uint64_t>(row) * kDumpRowSamples,
             ch.samples + row * kDumpRowSamples);
    ++g_rowCursor;
  }
  return g_rowCursor >= g_rowsTotal;
}

// The window is out: record again, and hold the next trigger back for the
// cooldown so one exposure does not print window after window.
void endDump() {
  const char end[] = "!dump end\n";
  writeLine(end, sizeof(end) - 1);
  capture::historyResume();
  g_dumpPhase = DumpPhase::kIdle;
  g_cooldown = kDumpCooldownChunks;
}
}  // namespace

void countResult(const scan::Result& result) {
  switch (result.status) {
    case scan::Status::kOk: ++g_ok; break;
    case scan::Status::kStale: ++g_stale; break;
    case scan::Status::kClipped: ++g_clip; break;
    case scan::Status::kWeak: ++g_weak; break;
  }
  g_lastStatus = result.status;
  g_lastNs = result.exposureNs;
  g_haveResult = true;
  // A measurement is the trigger: the raw signal behind it is what the dump
  // is for (issue 16).
  armDump();
}

void dumpDue(bool chunkScanned) {
  // Chunk work: the idle clock, the cooldown and the post-roll all count
  // scanned chunks, not loop iterations.
  if (chunkScanned) {
    ++g_dumpChunks;
    if (g_cooldown != 0) --g_cooldown;
    switch (g_dumpPhase) {
      case DumpPhase::kIdle:
        // No event at all: print the quiet baseline now and then, so the
        // ripple of the light source is visible without a shutter.
        if (kDumpIdleChunks != 0 && g_dumpChunks % kDumpIdleChunks == 0) {
          armDump();
        }
        break;
      case DumpPhase::kArmed:
        // The post-roll: the ring records the halves after the trigger, so the
        // window also holds what came after the measurement.
        if (g_postRoll != 0) --g_postRoll;
        if (g_postRoll == 0) startDump();
        break;
      case DumpPhase::kPrinting:
        break;
    }
  }
  // The rows go out on every pass, also when no chunk arrives: a stalled
  // capture must not hold back a window that is already frozen.
  if (g_dumpPhase == DumpPhase::kPrinting && printDumpRows()) endDump();
}

void reportDue(bool chunkScanned) {
  if (!chunkScanned) return;
  ++g_chunks;
  trackRunExtremes();
  if (g_chunks % kDebugChunkInterval != 0) return;

  if (!g_legendPrinted) {
    g_legendPrinted = true;
    printLegend();
  }
  printReport();
}

}  // namespace debug
#endif  // defined(ARDUINO) && defined(SHUTTERCHECK_DEBUG)