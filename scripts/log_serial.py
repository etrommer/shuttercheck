#!/usr/bin/env python3
"""Print the USB CDC output on stdout and append it to a log file.

Usage:
    python scripts/log_serial.py [--port /dev/ttyACM0] [--log capture.log]
                                 [--duration 60] [--timestamp]

The board prints one measurement per line. `pio device monitor` needs an
interactive terminal, so this script opens the port with pyserial instead.
It echoes every line and writes it to the log until Ctrl-C or until
`--duration` expires. It flushes after every line, so the log stays readable
while the capture runs.

The log also holds the log scripts cannot print: none. Everything the board
sends reaches both sinks unchanged.
"""

import argparse
import glob
import sys
import time

import serial

BAUD = 115200  # USB CDC ignores the rate, but the host still sets one.


def find_port():
    for pattern in ("/dev/ttyACM*", "/dev/ttyUSB*"):
        ports = sorted(glob.glob(pattern))
        if ports:
            return ports[0]
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", help="USB CDC port. Auto-detected by default.")
    parser.add_argument("--log", default="capture.log", help="Log file.")
    parser.add_argument("--duration", type=float, default=0.0,
                        help="Seconds to log. 0 means until Ctrl-C.")
    parser.add_argument("--timestamp", action="store_true",
                        help="Prefix every line with the host wall clock.")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("No USB CDC port found. Pass one with --port.", file=sys.stderr)
        return 2

    deadline = time.monotonic() + args.duration if args.duration else None
    print("== logging %s to %s (Ctrl-C to stop)" % (port, args.log))

    try:
        with serial.Serial(port, BAUD, timeout=1) as ser, \
                open(args.log, "a", buffering=1, newline="") as log:
            # One bulk read per pass. A raw dump sends about 20 kB in a few
            # seconds; one byte per read() call cannot keep up, and the bytes
            # it misses land as a hole in the middle of a dump.
            pending = b""
            while deadline is None or time.monotonic() < deadline:
                chunk = ser.read(65536)
                if not chunk:
                    continue
                pending += chunk
                if b"\n" not in pending:
                    continue
                *lines, pending = pending.split(b"\n")
                for raw in lines:
                    line = raw.decode("utf-8", "replace").rstrip("\r")
                    if args.timestamp:
                        line = "%s %s" % (time.strftime("%H:%M:%S"), line)
                    print(line)
                    log.write(line + "\n")
    except KeyboardInterrupt:
        pass
    except serial.SerialException as exc:
        print("Serial error: %s" % exc, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
