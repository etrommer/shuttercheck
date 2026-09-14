// Shutter edge scan and exposure measurement (issue 5).
//
// Pure integer, stdint only: this file is compiled both by the firmware and
// by the native unit-test binary, and must stay free of HAL and Arduino
// headers. The scan rebuilds the two shutter edges from one finished buffer
// half, measures the exposure in nanoseconds and pushes a result FIFO. The
// state and one overlap sample cross the halves.
//
// Boot: the very first sample seeds the dark plateau, and the bright plateau
// starts one small step above it (the shutter is closed at power-on, so the
// signal is at its dark level). The midpoint of the two plateaus is the
// threshold; the hysteresis band is max(8 LSB, span/16).
#include "scan.h"

namespace scan {

namespace {
constexpr int32_t kInitSpread = 16;   // Boot bright estimate above dark (LSB).
constexpr int32_t kEmaK = 64;         // Plateau EMA lambda (power of two).
constexpr int32_t kFullScale = 4095;  // 12-bit full count.
constexpr int32_t kClippedMargin = 32;
constexpr int32_t kWeakMinSpan = 48;
constexpr uint32_t kPeriodNs = 2000;
}  // namespace

namespace {
inline int32_t currentThr(const State& s) {
  return (s.dark + s.bright) / 2;
}
inline int32_t currentBand(const State& s) {
  int32_t band = (s.bright - s.dark) / 16;
  if (band < 8) band = 8;
  return band;
}
// Linear interpolation of a crossing time between v0 and v1 at `level`, per
// the spec: t = (n-1)*2us + (level - v_{n-1})/(v_n - v_{n-1}) * 2us.
inline int64_t interpolateNs(int32_t v0, int32_t v1, int32_t level,
                             uint64_t index) {
  int32_t lo = v0 < v1 ? v0 : v1;
  int32_t hi = v0 < v1 ? v1 : v0;
  int64_t frac = (int64_t)(level - lo) * kPeriodNs / (int64_t)(hi - lo);
  return (int64_t)(index - 1) * kPeriodNs + frac;
}
inline Status classify(const State& s) {
  // Clipped is decided on the raw excursion, not the plateau estimate: the
  // integer EMA stalls about K LSB below a plateau (its increment floors to
  // zero), so it can never approach full scale closely enough to signal a
  // rail hit. A latch on the raw samples is exact.
  if (s.hitRail) return Status::kClipped;
  if (s.bright - s.dark < kWeakMinSpan) return Status::kWeak;
  return Status::kOk;
}
}  // namespace

void initState(State* s) { *s = State{}; }

void fifoInit(ResultFifo* f) { f->head = 0; f->tail = 0; }

void scan(const uint16_t* samples, uint32_t count, State* s,
          ResultFifo* fifo) {
  // Hoist the state into a local struct for the whole chunk. The compiler
  // cannot prove `samples` does not alias `*s`, so field access through `s`
  // would touch RAM on every sample. The hot fields live in registers
  // instead; only the chunk edges copy memory.
  State l = *s;
  uint32_t k = 0;
  while (k < count) {
    int32_t v = samples[k];

    // First sample ever: seed the plateaus and the overlap sample, then move
    // on. The shutter is closed, so the first sample is the dark level.
    if (!l.havePrev) {
      l.prev = v;
      l.havePrev = 1;
      l.dark = v;
      l.initDark = 1;
      l.bright = v + kInitSpread;
      l.initBright = 1;
      l.nextIndex = 1;
      l.prevInBand = 0;
      ++k;
      continue;
    }

    // Quiet-run fast path: when no pulse is open, runs of samples that
    // provably change nothing but the index are skipped whole. Two disjoint
    // cases (prev is either strictly inside the band, or exactly on a
    // plateau; the band and the plateaus cannot move during either run):
    //  A. prev strictly inside the band -> every strictly-in-band sample is
    //     EMA-excluded and cannot cross: a rise needs prev at/below the low
    //     edge, a stale fall needs prev at/above the high edge. The run's
    //     last sample becomes prev; prevInBand stays 1.
    //  B. prev sits exactly on dark or bright -> each equal sample leaves
    //     the EMA at its value (the update is a no-op) and cannot cross.
    //     prev and prevInBand stay put.
    // hitRail cannot latch either (in-band and plateau values stay far below
    // the rail, and no pulse is open anyway). Samples at the exact band
    // edges stay on the full path: prev at an edge is an armed state.
    int32_t thr = currentThr(l);
    int32_t band = currentBand(l);
    int32_t half = band / 2;
    int32_t lowEdge = thr - half;
    int32_t highEdge = thr + half;
    if (!l.inPulse) {
      bool common = false;
      // Order the tests hot-first: with a pulse open (common on a busy
      // signal) the gate exits on the very first test.
      if (l.prevInBand && l.prev > lowEdge && l.prev < highEdge &&
          v > lowEdge && v < highEdge) {
        // Case A: run of strictly-in-band samples.
        common = true;
        uint32_t run = 1;
        while (k + run < count) {
          int32_t w = samples[k + run];
          if (w <= lowEdge || w >= highEdge) break;
          ++run;
        }
        l.prev = samples[k + run - 1];
        l.nextIndex += run;
        k += run;
      } else if ((l.prev == l.dark || l.prev == l.bright) && v == l.prev) {
        // Case B: run of samples equal to the plateau.
        common = true;
        uint32_t run = 1;
        while (k + run < count && samples[k + run] == l.prev) ++run;
        l.nextIndex += run;
        k += run;
      }
      if (common) continue;
    }

    bool curInBand = (v >= lowEdge) && (v <= highEdge);

    bool rising = false;
    bool falling = false;
    bool crossed = false;

    // Clipped latch: while a pulse is open, any raw sample at/near the ADC
    // rail saturates the excursion and marks it untrusted.
    if (l.inPulse && v >= kFullScale - kClippedMargin) l.hitRail = 1;

    // Rising edge: only when the signal came back below the low band edge
    // (the re-arm). Lock the threshold for this excursion. `prev < thr` is
    // redundant: lowEdge = thr - half with half >= 4, so prev <= lowEdge
    // already implies prev < thr.
    if (l.prev <= lowEdge && v >= thr) {
      rising = true;
      crossed = true;
    } else if (l.inPulse) {
      // An open pulse closes when it falls back through the locked threshold.
      if (l.prev >= l.scanThr && v < l.scanThr) {
        falling = true;
        crossed = true;
      }
    } else {
      // A falling crossing with no open pulse is a lone falling crossing:
      // stale, dropped. `prev >= thr` is redundant: highEdge = thr + half
      // with half >= 4, so prev >= highEdge already implies prev >= thr.
      if (l.prev >= highEdge && v < thr) {
        falling = true;
        crossed = true;
      }
    }

    // Plateau tracking: the previous sample, only if it was outside the band
    // and did not straddle a crossing.
    if (!(l.prevInBand || crossed)) {
      if (l.prev <= lowEdge) {
        l.dark = (l.dark * (kEmaK - 1) + l.prev) / kEmaK;
      } else if (l.prev >= highEdge) {
        l.bright = (l.bright * (kEmaK - 1) + l.prev) / kEmaK;
      }
    }

    uint64_t index = l.nextIndex;
    if (rising) {
      l.scanThr = thr;
      l.inPulse = 1;
      l.hitRail = 0;
      l.riseNs = interpolateNs(l.prev, v, thr, index);
    } else if (falling) {
      if (l.inPulse) {
        int64_t fallNs = interpolateNs(l.prev, v, l.scanThr, index);
        Result r;
        r.status = classify(l);
        // Rejections print no value (README output: "0 clipped", "0 weak").
        r.exposureNs = (r.status == Status::kOk) ? (fallNs - l.riseNs) : 0;
        fifoPush(fifo, r);
        l.inPulse = 0;
      } else {
        Result r;
        r.status = Status::kStale;
        r.exposureNs = 0;
        fifoPush(fifo, r);
      }
    }

    l.prev = v;
    l.prevInBand = curInBand;
    ++l.nextIndex;
    ++k;
  }
  *s = l;
}

void fifoPush(ResultFifo* f, const Result& r) {
  // Single-producer (scan, in the DMA callback) single-consumer (loop). Drop
  // on overflow rather than overwrite an unconsumed measurement.
  uint32_t next = (f->tail + 1) % kFifoSize;
  if (next == f->head) return;
  f->buf[f->tail] = r;
  f->tail = next;
}

bool fifoPop(ResultFifo* f, Result* out) {
  if (f->head == f->tail) return false;
  *out = f->buf[f->head];
  f->head = (f->head + 1) % kFifoSize;
  return true;
}

}  // namespace scan