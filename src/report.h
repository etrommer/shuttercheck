// Shutter speed report (issue 28).
//
// Presentation only: this module turns one measured width into one line. It
// never touches the measurement path and it holds no state. Pure integer,
// stdint only, no HAL, no Arduino: the firmware and the native unit tests
// compile it. The printf of the firmware toolchain has no long long width, so
// the module writes into a caller buffer instead of printing.
#ifndef SHUTTERCHECK_REPORT_H
#define SHUTTERCHECK_REPORT_H

#include <stddef.h>
#include <stdint.h>

namespace report {

// One common shutter speed. `denom` is the mark as 1/denom s; denom == 0
// means whole seconds (`ns` is the mark then).
struct Mark {
  uint16_t denom;
  int64_t ns;  // Nominal width of the mark, in nanoseconds.
};

// The table, fastest mark first. A tie in the nearest-mark search goes to the
// faster mark.
constexpr uint32_t kMarkCount = 16;
const Mark* marks();

// The mark with the smallest error, and that error in permille of the
// nominal. The error is rounded, and it is signed: a positive value means
// that the measured width is longer than the mark. Both use the same number,
// so the printed mark and the printed error always agree.
const Mark& nearestMark(int64_t exposureNs);
int64_t errorPermille(int64_t exposureNs, const Mark& mark);

// Longest line this module can write. The terminator needs one byte more.
constexpr size_t kMaxLineBytes = 72;

// Writes "<measured> <mark> <error>" and, with `withRawNs`, the raw width as
// " ns=<exposureNs>". The result is always terminated. Returns the byte count
// without the terminator, so a short `cap` truncates.
size_t format(int64_t exposureNs, char* out, size_t cap, bool withRawNs);

}  // namespace report

#endif  // SHUTTERCHECK_REPORT_H
