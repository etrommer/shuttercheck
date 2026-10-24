#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["matplotlib>=3.8", "numpy>=1.24"]
# ///
"""Trace an ADC sample stream through the real scan and draw one SVG.

The script compiles the host harness in tools/trace_scan.cpp with the real
src/scan.cpp, runs it one sample at a time and plots three panels: the ADC
samples and the scan geometry, the excursion phase and the results.

Input JSON:
    {"polarity": "dark-high", "chunk": 512, "samples": [4000, 4000, 2048]}

`polarity` is `dark-high` (default) or `dark-low`. `chunk` is the plot
division, default 512. The scan itself does not depend on the chunk.

Run it with `uv run scripts/trace_scan.py input.json`, or with `python` after
`pip install matplotlib`.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.patches import Patch  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
HARNESS_SRC = ROOT / "tools" / "trace_scan.cpp"
SCAN_SRC = ROOT / "src" / "scan.cpp"
BUILD_DIR = ROOT / ".pio" / "trace_scan"
HARNESS = BUILD_DIR / "trace_scan"

PERIOD_NS = 2000
POLARITIES = ("dark-high", "dark-low")

PHASE_ORDER = (
    "dark-armed",
    "opening-candidate",
    "pulse-open",
    "closing-candidate",
    "stale-candidate",
)
PHASE_COLORS = {
    "dark-armed": "#e5e7eb",
    "opening-candidate": "#f59e0b",
    "pulse-open": "#10b981",
    "closing-candidate": "#3b82f6",
    "stale-candidate": "#ef4444",
}
STATUS_COLORS = {
    "ok": "#047857",
    "stale": "#b45309",
    "clipped": "#7c3aed",
    "weak": "#b91c1c",
}


def compile_harness() -> Path:
    """Compile the host harness into .pio/trace_scan and return its path."""
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get("CXX", "c++")
    subprocess.run(
        [
            cxx,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            f"-I{ROOT / 'src'}",
            str(HARNESS_SRC),
            str(SCAN_SRC),
            "-o",
            str(HARNESS),
        ],
        cwd=ROOT,
        check=True,
    )
    return HARNESS


def load_input(path: Path) -> tuple[list[int], str, int]:
    """Read and validate one input JSON file."""
    try:
        data = json.loads(path.read_text())
    except OSError as exc:
        raise ValueError(f"cannot read {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ValueError(f"{path} is not JSON: {exc}") from exc

    if not isinstance(data, dict):
        raise ValueError("the JSON root must be an object")
    samples = data.get("samples")
    if not isinstance(samples, list) or not samples:
        raise ValueError("'samples' must be a non-empty array")
    for pos, value in enumerate(samples):
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError(f"samples[{pos}] is not an integer")
        if not 0 <= value <= 4095:
            raise ValueError(f"samples[{pos}] is outside 0..4095")
    polarity = data.get("polarity", "dark-high")
    if polarity not in POLARITIES:
        raise ValueError("'polarity' must be 'dark-high' or 'dark-low'")
    chunk = data.get("chunk", 512)
    if isinstance(chunk, bool) or not isinstance(chunk, int) or chunk < 1:
        raise ValueError("'chunk' must be a positive integer")
    return samples, polarity, chunk


def run_trace(samples: list[int], polarity: str, harness: Path) -> list[dict]:
    """Run the harness and return one parsed JSON line per sample."""
    text = " ".join(str(value) for value in samples)
    proc = subprocess.run(
        [str(harness), polarity],
        input=text,
        text=True,
        capture_output=True,
        cwd=ROOT,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "the trace harness failed")

    trace = []
    for line_no, raw in enumerate(proc.stdout.splitlines(), start=1):
        if not raw.strip():
            continue
        try:
            line = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise RuntimeError(
                f"harness line {line_no} is not JSON: {exc}"
            ) from exc
        trace.append(line)

    if len(trace) != len(samples):
        raise RuntimeError(
            f"harness returned {len(trace)} lines for {len(samples)} samples"
        )
    for index, line in enumerate(trace):
        if line.get("index") != index:
            raise RuntimeError(
                f"harness index {line.get('index')} at line {index + 1}"
            )
    return trace


def _bin_last(x: np.ndarray, y: np.ndarray, max_points: int):
    """Return one value per bin: the last sample in the bin."""
    n = len(y)
    if n <= max_points:
        return x, y
    edges = np.linspace(0, n, max_points + 1, dtype=int)
    bx, by = [], []
    for start, stop in zip(edges[:-1], edges[1:]):
        if stop <= start:
            continue
        bx.append((start + stop - 1) / 2)
        by.append(y[stop - 1])
    return np.asarray(bx), np.asarray(by)


def _bin_max(y: np.ndarray, max_points: int):
    """Return the maximum and the center index of each bin."""
    n = len(y)
    if n <= max_points:
        return np.arange(n), y
    edges = np.linspace(0, n, max_points + 1, dtype=int)
    bx, by = [], []
    for start, stop in zip(edges[:-1], edges[1:]):
        if stop <= start:
            continue
        bx.append((start + stop - 1) / 2)
        by.append(y[start:stop].max())
    return np.asarray(bx), np.asarray(by)


def _bin_minmax(y: np.ndarray, max_points: int):
    """Return the minimum and maximum of each bin."""
    n = len(y)
    if n <= max_points:
        return np.arange(n), y, y
    edges = np.linspace(0, n, max_points + 1, dtype=int)
    bx, low, high = [], [], []
    for start, stop in zip(edges[:-1], edges[1:]):
        if stop <= start:
            continue
        segment = y[start:stop]
        bx.append((start + stop - 1) / 2)
        low.append(segment.min())
        high.append(segment.max())
    return np.asarray(bx), np.asarray(low), np.asarray(high)


def plot_trace(
    trace: list[dict],
    out: Path,
    *,
    title: str,
    polarity: str,
    chunk: int,
    max_points: int,
) -> None:
    """Draw the signal, phase and result panels for one trace."""
    n = len(trace)
    x = np.arange(n)
    raw = np.asarray([line["sample"] for line in trace], dtype=int)
    dark = np.asarray([line["dark"] for line in trace])
    bright = np.asarray([line["bright"] for line in trace])
    threshold = np.asarray([line["threshold"] for line in trace])
    low_edge = np.asarray([line["lowEdge"] for line in trace])
    high_edge = np.asarray([line["highEdge"] for line in trace])
    scan_thr = np.asarray([line["scanThr"] for line in trace])
    phases = [line["phase_after"] for line in trace]

    events = []
    for line in trace:
        for result in line["results"]:
            events.append(
                (
                    line["index"],
                    line["riseNs"],
                    result["status"],
                    result["exposureNs"],
                )
            )

    fig, (ax_sig, ax_phase, ax_res) = plt.subplots(
        3,
        1,
        sharex=True,
        figsize=(14, 9),
        gridspec_kw={"height_ratios": [4.2, 0.9, 1.7]},
    )

    # The signal and the geometry the scan uses.
    x_raw, raw_low, raw_high = _bin_minmax(raw, max_points)
    if n <= max_points:
        ax_sig.plot(
            x_raw,
            raw_low,
            color="#374151",
            linewidth=0.7,
            alpha=0.65,
            marker=".",
            markersize=2,
            label="raw sample",
        )
    else:
        ax_sig.fill_between(
            x_raw,
            raw_low,
            raw_high,
            step="mid",
            color="#374151",
            alpha=0.18,
            linewidth=0,
            label="raw sample",
        )
        ax_sig.plot(
            x_raw,
            (raw_low + raw_high) / 2,
            color="#374151",
            linewidth=0.6,
            alpha=0.6,
        )

    x_band, band_low = _bin_last(x, low_edge, max_points)
    _, band_high = _bin_last(x, high_edge, max_points)
    ax_sig.fill_between(
        x_band,
        band_low,
        band_high,
        step="mid",
        color="#fde68a",
        alpha=0.85,
        linewidth=0,
        label="hysteresis band",
    )

    for values, color, style, label in (
        (dark, "#1d4ed8", "--", "dark EMA"),
        (bright, "#047857", "--", "bright EMA"),
        (threshold, "#111827", "-.", "threshold"),
        (low_edge, "#a16207", ":", "band edges"),
        (high_edge, "#a16207", ":", None),
    ):
        x_line, y_line = _bin_last(x, values, max_points)
        ax_sig.plot(
            x_line,
            y_line,
            color=color,
            linestyle=style,
            linewidth=1.0,
            label=label,
        )

    x_lock, locked = _bin_max(scan_thr, max_points)
    locked = np.where(locked > 0, locked, np.nan)
    ax_sig.plot(
        x_lock,
        locked,
        color="#7c3aed",
        linestyle=":",
        linewidth=1.0,
        label="locked threshold",
    )

    ax_sig.set_ylabel("ADC code (12 bit)")
    ax_sig.set_ylim(-30, 4130)
    ax_sig.grid(axis="y", color="#e5e7eb", linewidth=0.6)
    ax_sig.legend(loc="upper right", fontsize=7, ncol=2, framealpha=0.9)

    # The phase strip. Runs keep the phase of the state after each sample.
    runs = []
    start = 0
    for pos in range(1, n):
        if phases[pos] != phases[start]:
            runs.append((start, pos - start, phases[start]))
            start = pos
    runs.append((start, n - start, phases[start]))
    for start, length, phase in runs:
        ax_phase.broken_barh(
            [(start, length)],
            (0, 1),
            facecolors=PHASE_COLORS.get(phase, "#9ca3af"),
            edgecolors="none",
        )
        if length >= max(4, n * 0.06):
            ax_phase.text(
                start + length / 2,
                0.5,
                phase,
                ha="center",
                va="center",
                fontsize=6,
                color="#111827",
                clip_on=True,
            )

    changes = [pos for pos in range(1, n) if phases[pos] != phases[pos - 1]]
    if changes:
        ax_phase.plot(
            changes,
            [0.5] * len(changes),
            linestyle="none",
            marker="|",
            color="#111827",
            markersize=5,
            alpha=0.45,
        )
    ax_phase.set_ylim(0, 1)
    ax_phase.set_yticks([])
    ax_phase.set_ylabel("phase")

    # The results. An ok result is a bar over the measured width; a
    # rejection is a mark at the sample that published it.
    if events:
        for pos, (index, rise_ns, status, exposure_ns) in enumerate(events):
            if status == "ok" and exposure_ns > 0:
                ax_res.barh(
                    pos,
                    exposure_ns / PERIOD_NS,
                    left=rise_ns / PERIOD_NS,
                    height=0.45,
                    color="#10b981",
                    alpha=0.75,
                    edgecolor="none",
                )
                label = f"{status} {exposure_ns / 1000:.2f} us"
                color = STATUS_COLORS["ok"]
            else:
                ax_res.scatter(
                    index,
                    pos,
                    marker="x",
                    s=45,
                    color=STATUS_COLORS.get(status, "#111827"),
                )
                label = status
                color = STATUS_COLORS.get(status, "#111827")

            tx = index + 1
            align = "left"
            if tx > n * 0.9:
                tx = index - 1
                align = "right"
            ax_res.text(
                tx,
                pos,
                label,
                ha=align,
                va="center",
                fontsize=7,
                color=color,
            )

        ax_res.set_yticks(range(len(events)))
        ax_res.set_yticklabels(
            [f"result {pos}" for pos in range(len(events))], fontsize=7
        )
        ax_res.set_ylim(len(events) - 0.5, -0.5)
    else:
        ax_res.text(
            0.5,
            0.5,
            "no results",
            transform=ax_res.transAxes,
            ha="center",
            va="center",
            fontsize=9,
            color="#6b7280",
        )
        ax_res.set_yticks([])

    ax_res.set_ylabel("result")
    ax_res.set_xlabel("sample index (2 us per sample)")
    ax_res.grid(axis="x", color="#e5e7eb", linewidth=0.5)

    for boundary in range(chunk, n, chunk):
        for axis in (ax_sig, ax_phase, ax_res):
            axis.axvline(
                boundary - 0.5,
                color="#9ca3af",
                linestyle=":",
                linewidth=0.6,
                alpha=0.7,
                zorder=0,
            )

    present = [phase for phase in PHASE_ORDER if phase in phases]
    if present:
        handles = [
            Patch(color=PHASE_COLORS[phase], label=phase) for phase in present
        ]
        fig.legend(
            handles=handles,
            loc="upper right",
            bbox_to_anchor=(1.0, 0.955),
            ncol=len(handles),
            fontsize=7,
            frameon=False,
        )

    fig.suptitle(
        f"Scan trace: {title} ({polarity}, {n} samples)",
        x=0.01,
        y=0.985,
        ha="left",
        va="top",
        fontsize=12,
    )
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, format="svg", bbox_inches="tight")
    plt.close(fig)

    # Matplotlib writes path data with trailing spaces. Strip them so the
    # generated file passes `git diff --check`.
    svg = out.read_text()
    out.write_text("\n".join(line.rstrip() for line in svg.splitlines()) + "\n")


def result_lines(trace: list[dict]):
    for line in trace:
        for result in line["results"]:
            yield line["index"], result


def check_trace(trace: list[dict]) -> str | None:
    """Return an error message for a failed check, or None."""
    results = list(result_lines(trace))
    if not results:
        return "the trace has no results"
    for index, result in results:
        if result["status"] == "ok" and result["exposureNs"] <= 0:
            return (
                f"ok result at sample {index} has width "
                f"{result['exposureNs']} ns"
            )
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="input JSON file")
    parser.add_argument(
        "--out", type=Path, help="output SVG (default: input name with .svg)"
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail when there is no result or an ok result has no width",
    )
    parser.add_argument(
        "--max-points",
        type=int,
        default=20000,
        help="maximum signal points per panel (default 20000)",
    )
    args = parser.parse_args(argv)

    if args.max_points < 1:
        print("trace_scan: --max-points must be positive", file=sys.stderr)
        return 2

    try:
        samples, polarity, chunk = load_input(args.input)
    except ValueError as exc:
        print(f"trace_scan: {exc}", file=sys.stderr)
        return 2

    try:
        harness = compile_harness()
        trace = run_trace(samples, polarity, harness)
    except (OSError, subprocess.CalledProcessError, RuntimeError) as exc:
        print(f"trace_scan: {exc}", file=sys.stderr)
        return 2

    results = list(result_lines(trace))
    if results:
        for index, result in results:
            print(
                f"{index:6d}  {result['status']:7s} "
                f"{result['exposureNs']} ns"
            )
    else:
        print("no results")

    out = args.out or args.input.with_suffix(".svg")
    try:
        plot_trace(
            trace,
            out,
            title=args.input.stem,
            polarity=polarity,
            chunk=chunk,
            max_points=args.max_points,
        )
    except OSError as exc:
        print(f"trace_scan: cannot write {out}: {exc}", file=sys.stderr)
        return 2

    print(f"wrote {out} ({len(trace)} samples)")
    if args.check:
        error = check_trace(trace)
        if error:
            print(f"trace_scan: check failed: {error}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
