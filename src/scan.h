// Shutter edge scan API (issue 5).
//
// Pure integer, stdint only: this header (with scan.cpp) is compiled both by
// the firmware and by the native unit-test binary. It must stay free of HAL
// and Arduino headers. The scan rebuilds the two shutter edges from one
// finished buffer half, measures the exposure in nanoseconds and pushes a
// small result FIFO. The state and one overlap sample cross the halves.
#ifndef SHUTTERCHECK_SCAN_H
#define SHUTTERCHECK_SCAN_H

#include <stdint.h>

namespace scan {

// Signal direction at PA1 (issue 10).
//
// The cascode front end inverts: with the shutter closed the cell is dark and
// the cascode output rests at the top of the ADC range, near full scale. With
// the shutter open the lit cell pulls the output down to V_bias, near
// 3.3 V / 2. The direct-coupled front end is the other way round.
//
// The polarity is a template parameter of scan(), never a run-time value, so
// the direction costs no cycles in the measurement loop (issue 10, 1a).
enum class Polarity : uint8_t {
  kDarkHigh = 0,  // Dark is the high plateau. The cascode front end.
  kDarkLow = 1,   // Dark is the low plateau. The direct-coupled front end.
};

// Samples per scanned chunk (== one DMA buffer half). The scan of one chunk
// must finish well inside its 1.024 ms period (design invariant 7).
constexpr uint32_t kChunkSamples = 512;

enum class Status : uint8_t {
  kOk = 0,       // one opening + one closing crossing, verified plateaus.
  kStale = 1,    // a crossing arrived without its partner of the same pulse.
  kClipped = 2,  // the rail-side plateau came within 32 LSB of full scale.
  kWeak = 3,     // plateau span below the 48 LSB noise floor.
};

inline const char* statusName(Status s) {
  switch (s) {
    case Status::kOk: return "ok";
    case Status::kStale: return "stale";
    case Status::kClipped: return "clipped";
    case Status::kWeak: return "weak";
  }
  return "?";
}

struct Result {
  Status status;
  int64_t exposureNs;  // Valid for kOk; 0 for rejections.
};

constexpr uint32_t kFifoSize = 16;

// Single-producer (the DMA callback / scan) single-consumer (loop) result
// FIFO. `head` and `tail` are volatile because two contexts share them.
struct ResultFifo {
  volatile uint32_t head;
  volatile uint32_t tail;
  Result buf[kFifoSize];
};
enum class ExcursionPhase : uint8_t {
  kDarkArmed,
  kOpeningCandidate,
  kPulseOpen,
  kClosingCandidate,
  kStaleCandidate,
};

// Plateau, hysteresis and excursion state carried across chunk scans, plus
// the one overlap sample (the last sample of the previous chunk).
struct State {
  int32_t prev;          // Last processed sample (the overlap sample).
  int32_t dark;          // Dark plateau EMA value (LSB).
  int32_t bright;        // Bright plateau EMA value (LSB).
  int32_t scanThr;       // Threshold locked for the current excursion.
  int64_t riseNs;        // First interpolated opening crossing time.
  int64_t candidateNs;   // First interpolated closing crossing time.
  uint64_t nextIndex;    // Absolute sample index of the next sample.
  uint8_t havePrev;      // prev is valid.
  uint8_t initDark;      // dark plateau initialized.
  uint8_t initBright;    // bright plateau initialized.
  ExcursionPhase phase;  // Current edge-qualification state.
  uint8_t prevInBand;    // prev lay inside the hysteresis band.
  uint8_t hitRail;       // A raw sample of the rail-side plateau (which one
                         // depends on Polarity) came within kClippedMargin of
                         // full scale.
#if defined(SHUTTERCHECK_DEBUG)
  // The raw extremes of the chunk in flight (issue 12). They are per chunk:
  // the report shows where one chunk sat, and the running extremes in
  // debug.cpp show where the whole run has been. Nothing in the scan reads
  // them, so a release build compiles exactly the loop of today.
  int32_t debugMin;  // Lowest raw sample of the chunk in flight.
  int32_t debugMax;  // Highest raw sample of the chunk in flight.
#endif
};

void initState(State* s);
void fifoInit(ResultFifo* f);
void fifoPush(ResultFifo* f, const Result& r);

// Scans `count` consecutive samples. `samples` is one finished buffer half;
// the absolute time base and the overlap sample live in `*s`. The nextIndex
// in `*s` advances by `count`, so callers only pass the newest finished chunk
// in time order. The DMA may fill the other half while this one is scanned;
// this half is quiescent, so a plain `const uint16_t*` is sufficient.
// Finished measurements go into `fifo`.
// `P` names the signal direction at PA1. The firmware compiles only the
// instantiation that matches its wiring; the native tests compile both.
template <Polarity P>
void scan(const uint16_t* samples, uint32_t count, State* s,
          ResultFifo* fifo);

// Pops one finished measurement. Returns false when the FIFO is empty. Runs
// from loop() only, never from an ISR.
bool fifoPop(ResultFifo* f, Result* out);

}  // namespace scan

#endif  // SHUTTERCHECK_SCAN_H