// Host-side scan trace (issue 32).
//
// Links the real src/scan.cpp and feeds one sample per call. `argv` selects
// the polarity. Reads whitespace-separated ADC codes from stdin and writes
// one JSON line per sample to stdout. The line holds the phase before and
// after the sample, the plateau and band state, the locked threshold and the
// results the scan published for that sample.
//
// This file is host-only. It must stay free of HAL and Arduino headers, like
// src/scan.cpp. Build it with the C++ compiler of the host; the Python front
// end does that.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "scan.h"

namespace {

const char* phaseName(scan::ExcursionPhase phase) {
  switch (phase) {
    case scan::ExcursionPhase::kDarkArmed: return "dark-armed";
    case scan::ExcursionPhase::kOpeningCandidate: return "opening-candidate";
    case scan::ExcursionPhase::kPulseOpen: return "pulse-open";
    case scan::ExcursionPhase::kClosingCandidate: return "closing-candidate";
    case scan::ExcursionPhase::kStaleCandidate: return "stale-candidate";
  }
  return "?";
}

template <scan::Polarity P>
int run() {
  scan::State state;
  scan::initState(&state);
  scan::ResultFifo fifo;
  scan::fifoInit(&fifo);

  int value = 0;
  while (std::cin >> value) {
    if (value < 0 || value > 4095) {
      std::fprintf(stderr, "trace_scan: sample %d is outside 0..4095\n",
                   value);
      return 2;
    }

    const uint64_t index = state.nextIndex;
    const scan::ExcursionPhase before = state.phase;
    const uint16_t sample = static_cast<uint16_t>(value);
    scan::scan<P>(&sample, 1, &state, &fifo);
    const scan::Geometry g = scan::geometry<P>(state);

    std::printf(
        "{\"index\":%llu,\"sample\":%u,"
        "\"phase_before\":\"%s\",\"phase_after\":\"%s\","
        "\"dark\":%d,\"bright\":%d,\"threshold\":%d,"
        "\"lowEdge\":%d,\"highEdge\":%d,\"band\":%d,\"span\":%d,"
        "\"scanThr\":%d,\"riseNs\":%lld,\"candidateNs\":%lld,"
        "\"prev\":%d,\"prevInBand\":%u,\"havePrev\":%u,"
        "\"initDark\":%u,\"initBright\":%u,\"nextIndex\":%llu,"
        "\"results\":[",
        static_cast<unsigned long long>(index),
        static_cast<unsigned>(sample), phaseName(before),
        phaseName(state.phase), state.dark, state.bright, g.threshold,
        g.lowEdge, g.highEdge, g.band, g.span, state.scanThr,
        static_cast<long long>(state.riseNs),
        static_cast<long long>(state.candidateNs), state.prev,
        static_cast<unsigned>(state.prevInBand),
        static_cast<unsigned>(state.havePrev),
        static_cast<unsigned>(state.initDark),
        static_cast<unsigned>(state.initBright),
        static_cast<unsigned long long>(state.nextIndex));

    bool first = true;
    scan::Result result;
    while (scan::fifoPop(&fifo, &result)) {
      if (!first) std::putchar(',');
      first = false;
      std::printf("{\"status\":\"%s\",\"exposureNs\":%lld}",
                  scan::statusName(result.status),
                  static_cast<long long>(result.exposureNs));
    }
    std::printf("]}\n");
  }

  if (!std::cin.eof()) {
    std::fprintf(stderr, "trace_scan: stdin holds a non-integer value\n");
    return 2;
  }
  std::fflush(stdout);
  return 0;
}

void usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s dark-high|dark-low < samples.txt\n"
               "  One ADC code (0..4095) per value. Writes JSONL.\n",
               argv0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    usage(argv[0]);
    return 2;
  }
  if (std::strcmp(argv[1], "dark-high") == 0) {
    return run<scan::Polarity::kDarkHigh>();
  }
  if (std::strcmp(argv[1], "dark-low") == 0) {
    return run<scan::Polarity::kDarkLow>();
  }
  usage(argv[0]);
  return 2;
}
