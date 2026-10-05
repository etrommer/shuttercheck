#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["schemdraw==0.23"]
# ///
"""Generate the SFH 309 FA cascode schematic as an SVG.

Run it with `uv run docs/generate_schematic.py`. It rewrites
sfh309-cascode.svg in this directory.
"""

from pathlib import Path

import schemdraw
import schemdraw.elements as elm
import schemdraw.flow as flow

OUTPUT = Path(__file__).with_name("sfh309-cascode.svg")


def main():
    rail_y = 6.0
    base_y = 3.0

    with schemdraw.Drawing(file=str(OUTPUT), show=False) as d:
        d.config(unit=2.5, fontsize=10)

        # Cascode transistor Q2, its base driven by the V_bias tap.
        q2 = d.add(elm.BjtNpn(label="Q2\nBC547").at((2.4, base_y)))

        # The 3V3 rail across the top, ending where RL joins it.
        d.add(elm.Line().endpoints((0, rail_y), (q2.collector.x, rail_y)))
        d.add(elm.Vdd(label="3V3").at((0, rail_y)))

        # Bias divider: R1 from the rail to the V_bias tap, R2 to ground.
        d.add(elm.ResistorIEC(label="R1\n10 kΩ").endpoints((0, rail_y), (0, base_y)))
        d.add(elm.Dot().at((0, base_y)))
        d.add(elm.ResistorIEC(label="R2\n10 kΩ").endpoints((0, base_y), (0, 0)))
        d.add(elm.Ground().at((0, 0)))

        # The tap drives the base of Q2.
        d.add(elm.Line().endpoints((0, base_y), q2.base))

        # Load resistor RL from the output node at Q2's collector up to the
        # rail, and the output line to the ADC input of the STM32.
        d.add(elm.Dot().at(q2.collector))
        d.add(elm.ResistorIEC(label="RL\n10 kΩ").endpoints(q2.collector, (q2.collector.x, rail_y)))
        d.add(elm.Line().endpoints(q2.collector, (6.3, q2.collector.y)))
        d.add(flow.Box(label="STM32F103C8T6\nPA1 (ADC1_IN1)", w=3.8, h=1.9).at((6.3, q2.collector.y)).anchor("W"))

        # Phototransistor T1 below Q2: Q2's emitter into T1's collector,
        # T1's emitter to the ground rail. The light arrows are part of the
        # NpnPhoto symbol.
        t1 = d.add(elm.NpnPhoto(label="T1\nSFH 309 FA").at((q2.emitter.x, 1.9)).anchor("collector"))
        d.add(elm.Line().endpoints(q2.emitter, t1.collector))
        d.add(elm.Line().endpoints(t1.emitter, (t1.emitter.x, 0)))
        # The ground rail from R2's ground to T1's emitter.
        d.add(elm.Line().endpoints((0, 0), (t1.emitter.x, 0)))


if __name__ == "__main__":
    main()
