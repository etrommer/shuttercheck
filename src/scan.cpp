// Shutter edge scan and exposure measurement (issue 5).
//
// Pure integer, stdint only: this file is compiled both by the firmware and
// by the native unit-test binary, and must stay free of HAL and Arduino
// headers. The scan rebuilds the two shutter edges from one finished buffer
// half, measures the exposure in nanoseconds and pushes a result FIFO. The
// state and one overlap sample cross the halves.
//
// Boot: the very first sample seeds the dark plateau, and the bright plateau
// starts one small step away from it (the shutter is closed at power-on, so
// the signal is at its dark level). The midpoint of the two plateaus is the
// threshold; the hysteresis band is max(8 LSB, span/16).
//
// Polarity (issue 10): the cascode front end is inverting, so the dark
// plateau is the high one and the lit plateau sits at V_bias. Every polarity
// decision below is a compile-time constant on the template parameter P, so
// the scan loop costs the same in both directions.
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


// True when the dark plateau is the high plateau: the cascode front end.
template <Polarity P>
constexpr bool darkIsHigh() {
  return P == Polarity::kDarkHigh;
}
// True when an upward crossing of the threshold opens the shutter: the
// direct-coupled front end.
template <Polarity P>
constexpr bool upOpensShutter() {
  return !darkIsHigh<P>();
}
template <Polarity P>
inline int32_t currentThr(const State& s) {
  return (s.dark + s.bright) / 2;
}
// The plateau-to-plateau span, always positive and polarity-independent.
template <Polarity P>
inline int32_t plateauSpan(const State& s) {
  return darkIsHigh<P>() ? (s.dark - s.bright) : (s.bright - s.dark);
}
template <Polarity P>
inline int32_t currentBand(const State& s) {
  int32_t band = plateauSpan<P>(s) / 16;
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
template <Polarity P>
inline Status classify(const State& s) {
  // Clipped is decided on the raw excursion, not the plateau estimate: the
  // integer EMA stalls about K LSB below a plateau (its increment floors to
  // zero), so it can never approach full scale closely enough to signal a
  // rail hit. A latch on the raw samples is exact.
  if (s.hitRail) return Status::kClipped;
  if (plateauSpan<P>(s) < kWeakMinSpan) return Status::kWeak;
  return Status::kOk;
}
inline bool pulseIsOpen(const State& s) {
  return s.phase == ExcursionPhase::kPulseOpen ||
         s.phase == ExcursionPhase::kClosingCandidate;
}

// The rail latch: the front end can only drive its rail-side plateau to full
// scale, so a raw sample of the other plateau near full scale says nothing
// about this excursion. With kDarkLow the bright plateau is the rail side and
// its samples arrive inside the open pulse. With kDarkHigh the dark plateau
// is the rail side and its samples arrive while the shutter is closed, before
// the pulse opens. Both compile to one compare per sample.
template <Polarity P>
inline bool hitsRail(const State& l, int32_t v) {
  if (v < kFullScale - kClippedMargin) return false;
  return pulseIsOpen(l) == upOpensShutter<P>();
}

// Band geometry for one sample: the threshold and its hysteresis edges.
// The plateaus cannot move while a sample is processed, so the geometry is
// fixed for the whole iteration and can be computed once.
struct Band {
  int32_t thr;
  int32_t lowEdge;
  int32_t highEdge;
};

template <Polarity P>
inline Band bandOf(const State& s) {
  int32_t thr = currentThr<P>(s);
  int32_t half = currentBand<P>(s) / 2;
  return Band{thr, thr - half, thr + half};
}

inline bool inBand(int32_t v, const Band& b) {
  return v >= b.lowEdge && v <= b.highEdge;
}

// Seeding: the very first sample ever. The shutter is closed at power-on, so
// the sample is the dark level; the bright plateau starts one small step away
// from it (kInitSpread), on the side where the lit plateau will lie.
template <Polarity P>
inline void seedFirstSample(State* l, int32_t v) {
  l->prev = v;
  l->havePrev = 1;
  l->dark = v;
  l->initDark = 1;
  l->bright = v + (darkIsHigh<P>() ? -kInitSpread : kInitSpread);
  l->initBright = 1;
  l->prevInBand = 0;
}

#if defined(SHUTTERCHECK_DEBUG)
// The raw extremes of the chunk in flight, for the debug report (issue 12).
// The function is inlined and the two fields never feed the scan, so a
// release build compiles exactly the loop of today. Every path that
// consumes a sample calls it, also the skipped runs of skipQuietRun():
// their samples are samples, so the extremes must cover the whole chunk and
// not only the samples that reached the full path.
inline void trackExtremes(State* l, int32_t v) {
  if (v < l->debugMin) l->debugMin = v;
  if (v > l->debugMax) l->debugMax = v;
}
#endif

// Quiet-run fast path: when no pulse is open, runs of samples that provably
// change nothing but the index are skipped whole. Two disjoint cases (prev
// is either strictly inside the band, or exactly on a plateau; the band and
// the plateaus cannot move during either run):
//  A. prev strictly inside the band, on one side of the threshold -> every
//     strictly-in-band sample on that same side is EMA-excluded and cannot
//     cross. A run that reaches the threshold (or the other side) is left to
//     the full path, because it can hold an opening or a stale crossing.
//     The run's last sample becomes prev; prevInBand stays 1.
//  B. prev sits exactly on dark or bright -> each equal sample leaves the
//     EMA at its value (the update is a no-op) and cannot cross. prev and
//     prevInBand stay put.
// A run is skipped only when it cannot latch the rail flag either, so a
// saturated plateau still reaches the full path: case A needs highEdge below
// the rail zone, case B needs the plateau value below it. Both tests sit
// inside the branch they guard, so the full path pays nothing for them.
// Samples at the exact band edges stay on the full path: prev at an edge is
// an armed state.
// Returns true when a run was skipped.
template <Polarity P>
inline bool skipQuietRun(State* l, const uint16_t* samples, uint32_t* k,
                         uint32_t count, const Band& b) {
  if (l->phase != ExcursionPhase::kDarkArmed) return false;
  // Order the tests hot-first: with a pulse open (common on a busy signal)
  // the gate exits on the very first test.
  if (l->prevInBand && l->prev > b.lowEdge && l->prev < b.highEdge &&
      samples[*k] > b.lowEdge && samples[*k] < b.highEdge) {
    // Case A: run of strictly-in-band samples. A run that stays on one side
    // of the threshold holds no crossing; a run that reaches the threshold
    // can hold an opening crossing, so it stays on the full path.
    if (b.highEdge >= kFullScale - kClippedMargin) return false;
    const bool darkSideRun =
        darkIsHigh<P>() ? (l->prev > b.thr) : (l->prev < b.thr);
    const bool firstDark =
        darkIsHigh<P>() ? (samples[*k] > b.thr) : (samples[*k] < b.thr);
    if (firstDark != darkSideRun) return false;
    uint32_t run = 1;
    while (*k + run < count) {
      int32_t w = samples[*k + run];
      if (w <= b.lowEdge || w >= b.highEdge) break;
      if ((darkIsHigh<P>() ? (w > b.thr) : (w < b.thr)) != darkSideRun) break;
      ++run;
    }
#if defined(SHUTTERCHECK_DEBUG)
    // A skipped run is a run of samples, so the debug extremes cover it too
    // (issue 12). This costs one pass over the run in the debug build only.
    for (uint32_t j = *k; j < *k + run; ++j) trackExtremes(l, samples[j]);
#endif
    l->prev = samples[*k + run - 1];
    *k += run;
    return true;
  }
  if ((l->prev == l->dark || l->prev == l->bright) &&
      samples[*k] == l->prev) {
    if (l->prev >= kFullScale - kClippedMargin) return false;
    // Case B: run of samples equal to the plateau.
    uint32_t run = 1;
    while (*k + run < count && samples[*k + run] == l->prev) ++run;
#if defined(SHUTTERCHECK_DEBUG)
    for (uint32_t j = *k; j < *k + run; ++j) trackExtremes(l, samples[j]);
#endif
    *k += run;
    return true;
  }
  return false;
}

// Crossing kinds for one sample pair (prev -> v), in signal terms. The
// shutter meaning follows from the polarity: the shutter opens on the crossing
// that leaves the dark plateau.
enum class Crossing : uint8_t {
  kNone,
  kUp,      // The signal moved up across the threshold.
  kDown,    // The signal moved down across the threshold.
  kStale,   // A lone edge without an open pulse; confirm before reporting.
};
template <Polarity P>
inline bool onLightSide(int32_t v, int32_t thr) {
  return darkIsHigh<P>() ? (v <= thr) : (v >= thr);
}

template <Polarity P>
inline bool onDarkSide(int32_t v, int32_t thr) {
  return darkIsHigh<P>() ? (v > thr) : (v < thr);
}

template <Polarity P>
inline Crossing detectCrossing(const State& l, int32_t v, const Band& b) {
  // A crossing is a threshold straddle: prev on one side of the threshold,
  // the new sample on the other. The band must not gate the crossing itself.
  // If prev had to lie beyond the outer band edge, a slow edge (less than a
  // band width per sample) would slip through the band unseen, and the
  // exposure would never register. The band rejects in-band noise through
  // the two-sample confirmation that follows a crossing, not here.
  const bool prevOnDarkSide = onDarkSide<P>(l.prev, b.thr);
  const bool vOnLightSide = onLightSide<P>(v, b.thr);
  const Crossing opens = darkIsHigh<P>() ? Crossing::kDown : Crossing::kUp;
  const Crossing closes = darkIsHigh<P>() ? Crossing::kUp : Crossing::kDown;
  if (prevOnDarkSide && vOnLightSide) return opens;
  if (pulseIsOpen(l)) {
    // An open pulse closes when the signal falls back through its locked
    // threshold.
    const bool prevPastThr = darkIsHigh<P>() ? (l.prev <= l.scanThr)
                                             : (l.prev >= l.scanThr);
    const bool vBackOnDark = onDarkSide<P>(v, l.scanThr);
    return (prevPastThr && vBackOnDark) ? closes : Crossing::kNone;
  }
  // A lone return crossing is reported as stale only after confirmation. The
  // stale test keeps the band gate: after a pulse the plateaus have not
  // settled, so a slow edge that already closed the pulse would cross the
  // moving threshold again and report a second, false stale.
  const bool prevOnLightSide = darkIsHigh<P>() ? (l.prev <= b.lowEdge)
                                               : (l.prev >= b.highEdge);
  const bool vOnDarkSide = onDarkSide<P>(v, b.thr);
  if (prevOnLightSide && vOnDarkSide) return Crossing::kStale;
  return Crossing::kNone;
}

// Plateau tracking: carry the previous sample into its plateau EMA, only if
// it was outside the band and did not straddle a crossing. In-band samples
// are excluded as noise. The sample feeds the plateau on the side it sits on:
// the dark side is the high one only with kDarkHigh.
template <Polarity P>
inline void trackPlateaus(State* l, const Band& b, bool crossed) {
  if (l->prevInBand || crossed) return;
  if (l->prev <= b.lowEdge) {
    int32_t& lo = darkIsHigh<P>() ? l->bright : l->dark;
    lo = (lo * (kEmaK - 1) + l->prev) / kEmaK;
  } else if (l->prev >= b.highEdge) {
    int32_t& hi = darkIsHigh<P>() ? l->dark : l->bright;
    hi = (hi * (kEmaK - 1) + l->prev) / kEmaK;
  }
}

// Fast update for a known dark-side sample pair in kDarkArmed. The dark EMA
// and previous ADC sample both stay in 0..4095. The weighted sum is
// nonnegative, so an unsigned power-of-two divide preserves truncation.
inline void trackDarkPlateau(State* l) {
  if (!l->prevInBand) {
    const uint32_t weighted =
        static_cast<uint32_t>(l->dark) * (kEmaK - 1) +
        static_cast<uint32_t>(l->prev);
    l->dark = static_cast<int32_t>(weighted / kEmaK);
  }
}

// A threshold crossing starts a candidate. Confirmation uses later samples,
// but measurement time stays at this first interpolated crossing.
inline void startOpeningCandidate(State* l, int32_t v, const Band& b,
                                 uint64_t index) {
  l->scanThr = b.thr;
  l->riseNs = interpolateNs(l->prev, v, b.thr, index);
  l->phase = ExcursionPhase::kOpeningCandidate;
}

inline void startClosingCandidate(State* l, int32_t v, uint64_t index) {
  l->candidateNs = interpolateNs(l->prev, v, l->scanThr, index);
  l->phase = ExcursionPhase::kClosingCandidate;
}

// Confirmed closing edge: classify and publish the saved first-crossing time.
// The rail latch belongs to this excursion and is released after the result.
template <Polarity P>
inline void finishPulse(State* l, ResultFifo* fifo) {
  Result r;
  r.status = classify<P>(*l);
  r.exposureNs =
      r.status == Status::kOk ? (l->candidateNs - l->riseNs) : 0;
  fifoPush(fifo, r);
  l->phase = ExcursionPhase::kDarkArmed;
  l->hitRail = 0;
  l->candidateNs = 0;
}
}  // namespace

void initState(State* s) { *s = State{}; }

void fifoInit(ResultFifo* f) { f->head = 0; f->tail = 0; }

template <Polarity P>
void scan(const uint16_t* samples, uint32_t count, State* s,
          ResultFifo* fifo) {
  // Hoist the state into a local struct for the whole chunk. The compiler
  // cannot prove `samples` does not alias `*s`, so field access through `s`
  // would touch RAM on every sample. The hot fields live in registers
  // instead; only the chunk edges copy memory.
  State l = *s;
  const uint64_t chunkStart = l.nextIndex;
  uint32_t k = 0;
#if defined(SHUTTERCHECK_DEBUG)
  // The raw extremes are per chunk: they start again at the first sample of
  // every chunk (issue 12).
  if (count > 0) {
    l.debugMin = samples[0];
    l.debugMax = samples[0];
  }
#endif
  while (k < count) {
    // Seeding: the very first sample ever initializes the plateaus, then
    // move on.
    if (!l.havePrev) {
      seedFirstSample<P>(&l, samples[k]);
#if defined(SHUTTERCHECK_DEBUG)
      trackExtremes(&l, samples[k]);
#endif
      ++k;
      continue;
    }

    // Band geometry: fixed for this iteration (the plateaus cannot move
    // while one sample is processed).
    Band b = bandOf<P>(l);

    // Quiet-run fast path: skip whole runs that change nothing but the index.
    if (skipQuietRun<P>(&l, samples, &k, count, b)) continue;

    int32_t v = samples[k];
#if defined(SHUTTERCHECK_DEBUG)
    trackExtremes(&l, v);
#endif

    // During an open pulse, the previous sample is on the light side of the
    // locked threshold. Only the return crossing can occur in this phase.
    if (l.phase == ExcursionPhase::kPulseOpen) {
      if (upOpensShutter<P>() && v >= kFullScale - kClippedMargin) {
        l.hitRail = 1;
      }
      const bool crossed = darkIsHigh<P>()
                               ? (l.prev <= l.scanThr && v > l.scanThr)
                               : (l.prev >= l.scanThr && v < l.scanThr);
      const bool curInBand = inBand(v, b);
      trackPlateaus<P>(&l, b, crossed);
      if (crossed) startClosingCandidate(&l, v, chunkStart + k);
      l.prev = v;
      l.prevInBand = curInBand;
      ++k;
      continue;
    }

    // On the dark side in the armed phase, two consecutive samples cannot
    // open a pulse or form a stale edge. Keep plateau and rail tracking, but
    // skip the general crossing and phase logic for this common run.
    if (l.phase == ExcursionPhase::kDarkArmed) {
      const bool prevDarkSide = darkIsHigh<P>() ? l.prev >= b.highEdge
                                                 : l.prev <= b.lowEdge;
      const bool sampleDarkSide = darkIsHigh<P>() ? v >= b.highEdge
                                                   : v <= b.lowEdge;
      if (prevDarkSide && sampleDarkSide) {
        if (darkIsHigh<P>() && v >= kFullScale - kClippedMargin) {
          l.hitRail = 1;
        }
        const bool curInBand = darkIsHigh<P>() ? v == b.highEdge
                                                : v == b.lowEdge;
        trackDarkPlateau(&l);
        l.prev = v;
        l.prevInBand = curInBand;
        ++k;
        continue;
      }
    }

    // Latch rail samples from the active rail-side plateau.
    if (hitsRail<P>(l, v)) l.hitRail = 1;
    // Detect an edge or confirm a pending candidate before updating prev.
    // A candidate sample is excluded from plateau tracking, like a crossing.
    const ExcursionPhase phase = l.phase;
    const bool confirming = phase == ExcursionPhase::kOpeningCandidate ||
                            phase == ExcursionPhase::kClosingCandidate ||
                            phase == ExcursionPhase::kStaleCandidate;
    const Crossing c = confirming ? Crossing::kNone
                                  : detectCrossing<P>(l, v, b);
    const bool crossed = confirming || c != Crossing::kNone;
    const bool curInBand = inBand(v, b);
    trackPlateaus<P>(&l, b, crossed);

    switch (phase) {
      case ExcursionPhase::kDarkArmed:
        if (c == Crossing::kStale) {
          l.scanThr = b.thr;
          l.phase = ExcursionPhase::kStaleCandidate;
        } else if (c == (upOpensShutter<P>() ? Crossing::kUp
                                             : Crossing::kDown)) {
          startOpeningCandidate(&l, v, b, chunkStart + k);
        }
        break;
      case ExcursionPhase::kOpeningCandidate:
        l.phase = onLightSide<P>(v, l.scanThr)
                      ? ExcursionPhase::kPulseOpen
                      : ExcursionPhase::kDarkArmed;
        break;
      case ExcursionPhase::kPulseOpen:
        if (c != Crossing::kNone) {
          startClosingCandidate(&l, v, chunkStart + k);
        }
        break;
      case ExcursionPhase::kClosingCandidate:
        if (onDarkSide<P>(v, l.scanThr)) {
          finishPulse<P>(&l, fifo);
        } else {
          l.phase = ExcursionPhase::kPulseOpen;
          l.candidateNs = 0;
        }
        break;
      case ExcursionPhase::kStaleCandidate:
        if (onDarkSide<P>(v, l.scanThr)) {
          fifoPush(fifo, Result{Status::kStale, 0});
        }
        l.phase = ExcursionPhase::kDarkArmed;
        break;
    }

    l.prev = v;
    l.prevInBand = curInBand;
    ++k;
  }
  l.nextIndex = chunkStart + count;
  *s = l;
}

// The definition stays in this file, so the explicit instantiations below are
// the link entry points. Both polarities are emitted; the linker drops the one
// a build does not call, so the firmware carries only the polarity of its
// front end while the native tests link both.
template void scan<Polarity::kDarkHigh>(const uint16_t*, uint32_t, State*,
                                       ResultFifo*);
template void scan<Polarity::kDarkLow>(const uint16_t*, uint32_t, State*,
                                      ResultFifo*);

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