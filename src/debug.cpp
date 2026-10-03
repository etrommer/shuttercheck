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