#!/usr/bin/env python3
"""Build, flash and read the on-device self test (issue 8).

Usage:
    python scripts/on_device_test.py [--port /dev/ttyACM0] [--timeout 30] [--no-flash]

Steps:

1. Generate the scan test vectors (test/test_data.h).
2. Build and flash the `on_device_test` environment over SWD.
3. Open the USB CDC port and read the report the board prints.
4. Exit 0 when the board reports `ALL TESTS PASSED`, 1 when it reports
   failures, 2 on a timeout or a serial error.

It needs a board on SWD and on USB. GitHub CI does not run it.
"""

import argparse
import glob
import os
import subprocess
import sys
import time

import serial

ENV = "on_device_test"
PASS_LINE = "ALL TESTS PASSED"
FAIL_PREFIX = "FAILURES:"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd, cwd=ROOT):
    subprocess.run(cmd, cwd=cwd, check=True)


def find_port():
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
    return None


def read_report(port, timeout_s):
    """Read lines from `port` until the summary line. Returns (failed, lines)."""
    lines = []
    with serial.Serial(port, 115200, timeout=1) as ser:
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode(errors="replace").rstrip()
            print(line)
            lines.append(line)
            if line == PASS_LINE:
                return False, lines
            if line.startswith(FAIL_PREFIX):
                return True, lines
    return True, lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB CDC port. Auto-detected by default.")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="Seconds to wait for the report (default 30).")
    parser.add_argument("--no-flash", action="store_true",
                        help="Skip the build and flash; only read the port.")
    args = parser.parse_args()

    if not args.no_flash:
        print("== generating test vectors")
        run([sys.executable, "scripts/gen_test_data.py"])
        print("== building and flashing %s" % ENV)
        run(["pio", "run", "-e", ENV, "-t", "upload"])
        # The board re-enumerates after the flash.
        time.sleep(1.0)

    port = args.port or find_port()
    if not port:
        print("No USB CDC port found. Pass one with --port.", file=sys.stderr)
        return 2

    print("== reading %s" % port)
    try:
        failed, lines = read_report(port, args.timeout)
    except serial.SerialException as exc:
        print("Serial error: %s" % exc, file=sys.stderr)
        return 2

    if not lines:
        print("No report within %.0f s." % args.timeout, file=sys.stderr)
        return 2
    if failed:
        print("== FAILED", file=sys.stderr)
        return 1
    print("== PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
