// On-device self test (issue 8). Compiled into ON_DEVICE_TEST builds only.
//
// It runs the hardware checks and the generated scan cases, prints one line
// per check on the USB CDC port and returns the number of failures. The
// checks ask the HAL and the capture path. They read no peripheral registers
// and use no bit masks of their own.
#ifndef SHUTTERCHECK_SELFCHECK_H
#define SHUTTERCHECK_SELFCHECK_H

#include <stdint.h>

namespace selfcheck {

// Waits for the host to open the CDC port (at most 30 s), then runs every
// check and prints the report. Returns the number of failed checks.
uint32_t run();

}  // namespace selfcheck

#endif  // SHUTTERCHECK_SELFCHECK_H
