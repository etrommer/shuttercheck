#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib>=3.8", "numpy>=1.24"]
# ///
"""Build one scan trace plot for every generated test case and polarity.

The case list and the expected results come from scripts/gen_test_data.py.
The script mirrors each stream for `dark-high`, the same way
test/scan_cases.h does. It compiles the host harness once, runs every case,
compares the results with the expectations and writes
`test-plots/caseNN-dark-low.svg` and `test-plots/caseNN-dark-high.svg`.

The output directory is gitignored. CI uploads the files as an artifact.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_test_data  # noqa: E402
import trace_scan  # noqa: E402


def got_results(trace: list[dict]) -> list[tuple[str, int]]:
    """Return (status, exposureNs) in result order."""
    results = []
    for line in trace:
        for result in line["results"]:
            results.append((result["status"], result["exposureNs"]))
    return results


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        type=Path,
        default=Path("test-plots"),
        help="output directory (default: test-plots)",
    )
    parser.add_argument(
        "--max-points",
        type=int,
        default=20000,
        help="maximum signal points per panel (default 20000)",
    )
    args = parser.parse_args(argv)

    if args.max_points < 1:
        print("trace_test_cases: --max-points must be positive", file=sys.stderr)
        return 2

    try:
        harness = trace_scan.compile_harness()
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"trace_test_cases: cannot build the harness: {exc}",
              file=sys.stderr)
        return 2

    out_dir = args.out
    out_dir.mkdir(parents=True, exist_ok=True)
    failures = 0

    for index, (low, high, samples, expected_low, expected_high) in enumerate(
        gen_test_data.CASES
    ):
        for polarity, dark_high in (("dark-low", False), ("dark-high", True)):
            if dark_high:
                stream = [low + high - value for value in samples]
                expected = expected_high
            else:
                stream = list(samples)
                expected = expected_low

            try:
                trace = trace_scan.run_trace(stream, polarity, harness)
            except (OSError, RuntimeError) as exc:
                print(
                    f"trace_test_cases: case {index:02d} {polarity}: {exc}",
                    file=sys.stderr,
                )
                return 2

            got = got_results(trace)
            want = [(status, width) for width, status in expected]
            out = out_dir / f"case{index:02d}-{polarity}.svg"
            trace_scan.plot_trace(
                trace,
                out,
                title=f"case {index:02d}",
                polarity=polarity,
                chunk=gen_test_data.CHUNK,
                max_points=args.max_points,
            )

            if got != want:
                failures += 1
                print(
                    f"case {index:02d} {polarity}: MISMATCH",
                    file=sys.stderr,
                )
                print(f"  expected: {want}", file=sys.stderr)
                print(f"  got:      {got}", file=sys.stderr)
            else:
                print(
                    f"case {index:02d} {polarity}: {len(got)} result(s) "
                    f"-> {out}"
                )

    total = len(gen_test_data.CASES) * 2
    if failures:
        print(
            f"trace_test_cases: {failures} of {total} cases failed",
            file=sys.stderr,
        )
        return 1
    print(f"trace_test_cases: {total} plots in {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
