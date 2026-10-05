// Test pulse generator (issue 18).
//
// TIM2_CH1 on PA0 emits one light pulse per arm call, through the test
// divider to PA1, so the on-device self test can measure a known width with
// the real capture path. The pin idles dark (high) and the pulse is light
// (low): the same signal direction as `scan::Polarity::kDarkHigh` at PA1.
// Both pulse edges are timer events (the compare match and the update
// event), so the pulse has no software jitter and the CPU is idle while the
// timer runs.
#ifndef SHUTTERCHECK_PULSEGEN_H
#define SHUTTERCHECK_PULSEGEN_H

#include <stdint.h>

namespace pulsegen {

// Configures TIM2_CH1 on PA0 and holds the pin dark (high). No pulse runs
// before the first arm() call.
void begin();

// Emits one light pulse of `widthMs` milliseconds after 1 ms of dark
// lead-in. The lead-in gives the scan dark samples before the light edge.
// The call returns at once: the timer runs the pulse on its own.
// Valid range: 1 to 1300 ms. Wider pulses do not fit the 16-bit counter.
void arm(uint32_t widthMs);
// True while TIM2 is producing the armed pulse.
bool isRunning();

}  // namespace pulsegen

#endif  // SHUTTERCHECK_PULSEGEN_H
