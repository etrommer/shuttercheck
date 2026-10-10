// Shutter speed report (issue 28): the measured width, the nearest common
// shutter speed and the error of the measurement against that speed.
//
// Pure integer, stdint only, no HAL, no Arduino, no printf: the firmware and
// the native unit tests compile this file directly. The measurement path
// never calls back into this module, and this module never changes a width.
#include "report.h"

namespace report {
namespace {

constexpr int64_t kSecondNs = 1000000000LL;
// Below this width the line shows the fraction form. 1/1.09 s reads badly, so
// the slower values use the seconds form.
constexpr int64_t kFractionLimitNs = 500000000LL;
// The percent scale must not overflow. A width above about 1e6 s cannot come
// from a shutter, so the search and the error clamp there.
constexpr int64_t kMaxWidthNs = 1000000000000000LL;

const Mark kMarks[kMarkCount] = {
    {4000, 250000LL},        // 1/4000 s
    {2000, 500000LL},        // 1/2000 s
    {1000, 1000000LL},       // 1/1000 s
    {500, 2000000LL},        // 1/500 s
    {250, 4000000LL},        // 1/250 s
    {125, 8000000LL},        // 1/125 s
    {60, 16666666LL},        // 1/60 s
    {30, 33333333LL},        // 1/30 s
    {15, 66666666LL},        // 1/15 s
    {8, 125000000LL},        // 1/8 s
    {4, 250000000LL},        // 1/4 s
    {2, 500000000LL},        // 1/2 s
    {0, 1000000000LL},       // 1 s
    {0, 2000000000LL},       // 2 s
    {0, 4000000000LL},       // 4 s
    {0, 8000000000LL},       // 8 s
};

int64_t widthOf(int64_t exposureNs) {
  if (exposureNs < 1) return 1;  // An accepted pulse always carries a width.
  if (exposureNs > kMaxWidthNs) return kMaxWidthNs;
  return exposureNs;
}

uint64_t pow10(uint32_t exp) {
  uint64_t v = 1;
  while (exp-- != 0) v *= 10;
  return v;
}

// Appends `v` in decimal. Every append stops at the buffer limit, so a wrong
// field width can shorten the line but never write past it.
size_t appendUint(char* out, size_t n, size_t cap, uint64_t v) {
  char tmp[20];
  size_t len = 0;
  do {
    tmp[len++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);
  while (len != 0 && n + 1 < cap) out[n++] = tmp[--len];
  return n;
}

size_t appendChar(char* out, size_t n, size_t cap, char c) {
  if (n + 1 < cap) out[n++] = c;
  return n;
}

// Appends num/den with three significant digits: 403, 60.6, 2.48, 205000.
// The value is at least 1 here. `num` and `den` keep the full precision of
// the division, so the third digit is rounded, not truncated.
size_t appendThreeDigits(char* out, size_t n, size_t cap, uint64_t num,
                         uint64_t den) {
  uint64_t whole = num / den;
  uint64_t rem = num % den;
  int32_t digits = 0;
  for (uint64_t t = whole; t != 0; t /= 10) ++digits;
  if (digits == 0) digits = 1;
  const int32_t extra = 3 - digits;

  if (extra >= 0) {
    // The integer part is shorter than three digits: print `extra` decimals.
    uint64_t frac = 0;
    uint64_t roundDigit = 0;
    for (int32_t i = 0; i <= extra; ++i) {
      rem *= 10;
      const uint64_t d = rem / den;
      rem %= den;
      if (i < extra) {
        frac = frac * 10 + d;
      } else {
        roundDigit = d;
      }
    }
    if (roundDigit >= 5) {
      ++frac;
      if (frac == pow10(static_cast<uint32_t>(extra))) {
        frac = 0;
        ++whole;
      }
    }
    // Trailing zeros are not significant: an exact 1/8 s prints "8", a
    // rounded 1/60 s prints "60", and 1/54.2 s keeps its one digit.
    int32_t shown = extra;
    while (shown > 0 && frac % 10 == 0) {
      frac /= 10;
      --shown;
    }
    n = appendUint(out, n, cap, whole);
    if (shown > 0) {
      n = appendChar(out, n, cap, '.');
      uint64_t place = pow10(static_cast<uint32_t>(shown) - 1);
      for (int32_t i = 0; i < shown; ++i) {
        n = appendChar(out, n, cap,
                       static_cast<char>('0' + (frac / place) % 10));
        place /= 10;
      }
    }
    return n;
  }

  // The integer part is longer than three digits: drop the tail digits and
  // round the last kept digit.
  const uint64_t scale = pow10(static_cast<uint32_t>(-extra));
  const uint64_t head = whole / scale;
  const uint64_t tail = whole % scale;
  const bool up = (tail * 2 > scale) || (tail * 2 == scale && rem * 2 >= den);
  return appendUint(out, n, cap, (head + (up ? 1 : 0)) * scale);
}

size_t appendPermille(char* out, size_t n, size_t cap, int64_t permille) {
  if (permille < 0) {
    n = appendChar(out, n, cap, '-');
    permille = -permille;
  } else if (permille > 0) {
    n = appendChar(out, n, cap, '+');
  }
  n = appendUint(out, n, cap, static_cast<uint64_t>(permille) / 10);
  n = appendChar(out, n, cap, '.');
  return appendChar(out, n, cap,
                    static_cast<char>('0' + permille % 10));
}

}  // namespace

const Mark* marks() { return kMarks; }

int64_t errorPermille(int64_t exposureNs, const Mark& mark) {
  const int64_t width = widthOf(exposureNs);
  const int64_t diff = width > mark.ns ? width - mark.ns : mark.ns - width;
  // Two terms, so the scaled remainder stays small and nothing overflows.
  int64_t permille = (diff / mark.ns) * 1000;
  permille += ((diff % mark.ns) * 1000 + mark.ns / 2) / mark.ns;
  return width >= mark.ns ? permille : -permille;
}

const Mark& nearestMark(int64_t exposureNs) {
  // The table runs fastest first and the compare is strict, so a tie goes to
  // the faster mark.
  const Mark* best = &kMarks[0];
  int64_t bestError = errorPermille(exposureNs, *best);
  if (bestError < 0) bestError = -bestError;
  for (uint32_t i = 1; i < kMarkCount; ++i) {
    int64_t error = errorPermille(exposureNs, kMarks[i]);
    if (error < 0) error = -error;
    if (error < bestError) {
      bestError = error;
      best = &kMarks[i];
    }
  }
  return *best;
}

size_t format(int64_t exposureNs, char* out, size_t cap, bool withRawNs) {
  if (out == nullptr || cap < 1) return 0;
  const int64_t width = widthOf(exposureNs);
  const Mark& mark = nearestMark(exposureNs);

  char line[kMaxLineBytes];
  const size_t limit = sizeof(line);
  size_t n = 0;

  if (width < kFractionLimitNs) {
    // 1/n s, three significant digits.
    n = appendChar(line, n, limit, '1');
    n = appendChar(line, n, limit, '/');
    n = appendThreeDigits(line, n, limit, static_cast<uint64_t>(kSecondNs),
                          static_cast<uint64_t>(width));
    n = appendChar(line, n, limit, ' ');
    n = appendChar(line, n, limit, 's');
  } else {
    // Whole seconds with two decimals.
    uint64_t secs = static_cast<uint64_t>(width) / kSecondNs;
    const uint64_t rem = static_cast<uint64_t>(width) % kSecondNs;
    uint64_t hundredths = (rem * 100 + kSecondNs / 2) / kSecondNs;
    if (hundredths >= 100) {
      hundredths -= 100;
      ++secs;
    }
    n = appendUint(line, n, limit, secs);
    n = appendChar(line, n, limit, '.');
    n = appendChar(line, n, limit,
                   static_cast<char>('0' + hundredths / 10));
    n = appendChar(line, n, limit,
                   static_cast<char>('0' + hundredths % 10));
    n = appendChar(line, n, limit, ' ');
    n = appendChar(line, n, limit, 's');
  }

  n = appendChar(line, n, limit, ' ');
  if (mark.denom != 0) {
    n = appendChar(line, n, limit, '1');
    n = appendChar(line, n, limit, '/');
    n = appendUint(line, n, limit, mark.denom);
  } else {
    n = appendUint(line, n, limit,
                   static_cast<uint64_t>(mark.ns) / kSecondNs);
    n = appendChar(line, n, limit, ' ');
    n = appendChar(line, n, limit, 's');
  }

  n = appendChar(line, n, limit, ' ');
  n = appendPermille(line, n, limit, errorPermille(exposureNs, mark));
  n = appendChar(line, n, limit, '%');

  if (withRawNs) {
    n = appendChar(line, n, limit, ' ');
    n = appendChar(line, n, limit, 'n');
    n = appendChar(line, n, limit, 's');
    n = appendChar(line, n, limit, '=');
    n = appendUint(line, n, limit, static_cast<uint64_t>(exposureNs));
  }

  line[n] = '\0';
  const size_t copy = n < cap - 1 ? n : cap - 1;
  for (size_t i = 0; i < copy; ++i) out[i] = line[i];
  out[copy] = '\0';
  return copy;
}

}  // namespace report
