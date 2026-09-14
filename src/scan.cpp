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
  for (uint32_t k = 0; k < count; ++k) {
    int32_t v = samples[k];

    // First sample ever: seed the plateaus and the overlap sample, then move
    // on. The shutter is closed, so the first sample is the dark level.
    if (!s->havePrev) {
      s->prev = v;
      s->havePrev = 1;
      s->dark = v;
      s->initDark = 1;
      s->bright = v + kInitSpread;
      s->initBright = 1;
      s->nextIndex = 1;
      s->prevInBand = 0;
      continue;
    }

    int32_t thr = currentThr(*s);
    int32_t band = currentBand(*s);
    int32_t half = band / 2;
    int32_t lowEdge = thr - half;
    int32_t highEdge = thr + half;
    bool curInBand = (v >= lowEdge) && (v <= highEdge);

    bool rising = false;
    bool falling = false;
    bool crossed = false;

    // Clipped latch: while a pulse is open, any raw sample at/near the ADC
    // rail saturates the excursion and marks it untrusted.
    if (s->inPulse && v >= kFullScale - kClippedMargin) s->hitRail = 1;

    // Rising edge: only when the signal came back below the low band edge
    // (the re-arm). Lock the threshold for this excursion.
    if (s->prev <= lowEdge && s->prev < thr && v >= thr) {
      rising = true;
      crossed = true;
    } else if (s->inPulse) {
      // An open pulse closes when it falls back through the locked threshold.
      if (s->prev >= s->scanThr && v < s->scanThr) {
        falling = true;
        crossed = true;
      }
    } else {
      // A falling crossing with no open pulse is a lone falling crossing:
      // stale, dropped.
      if (s->prev >= highEdge && s->prev >= thr && v < thr) {
        falling = true;
        crossed = true;
      }
    }

    // Plateau tracking: the previous sample, only if it was outside the band
    // and did not straddle a crossing.
    if (!(s->prevInBand || crossed)) {
      if (s->prev <= lowEdge) {
        s->dark = (s->dark * (kEmaK - 1) + s->prev) / kEmaK;
      } else if (s->prev >= highEdge) {
        s->bright = (s->bright * (kEmaK - 1) + s->prev) / kEmaK;
      }
    }

    uint64_t index = s->nextIndex;
    if (rising) {
      s->scanThr = thr;
      s->inPulse = 1;
      s->hitRail = 0;
      s->riseNs = interpolateNs(s->prev, v, thr, index);
    } else if (falling) {
      if (s->inPulse) {
        int64_t fallNs = interpolateNs(s->prev, v, s->scanThr, index);
        Result r;
        r.status = classify(*s);
        // Rejections print no value (README output: "0 clipped", "0 weak").
        r.exposureNs = (r.status == Status::kOk) ? (fallNs - s->riseNs) : 0;
        fifoPush(fifo, r);
        s->inPulse = 0;
      } else {
        Result r;
        r.status = Status::kStale;
        r.exposureNs = 0;
        fifoPush(fifo, r);
      }
    }

    s->prev = v;
    s->prevInBand = curInBand;
    ++s->nextIndex;
  }
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