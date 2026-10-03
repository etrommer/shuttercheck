#!/usr/bin/env -S uv run --script
# /// script
# dependencies = ["svg-schematic==1.3"]
# ///
"""Generate the SFH309 FA cascode schematic as an SVG."""

from pathlib import Path

from svg_schematic import BJT, Box, Capacitor, Dot, Ground, Resistor, Schematic, Wire


OUTPUT = Path(__file__).with_name("sfh309-cascode.svg")


def add_light_arrow(schematic, start, tip):
    """Add one light arrow directed at the phototransistor symbol."""
    schematic.add(
        schematic.line(
            start=start,
            end=tip,
            stroke="black",
            stroke_width=2,
            stroke_linecap="round",
        )
    )
    x, y = tip
    schematic.add(
        schematic.polyline(
            [(x - 9, y - 2), (x, y), (x - 2, y - 9)],
            fill="none",
            stroke="black",
            stroke_width=2,
            stroke_linecap="round",
            stroke_linejoin="round",
        )
    )


def add_label(schematic, text, position):
    schematic.add(
        schematic.text(
            text,
            insert=position,
            font_family="sans-serif",
            font_size=16,
            text_anchor="middle",
            fill="black",
        )
    )


def main():
    with Schematic(
        filename=str(OUTPUT),
        font_family="sans-serif",
        font_size=16,
        line_width=2,
        dot_radius=4,
        pad=45,
    ) as schematic:
        # Build the components first so the wires can use their pin coordinates.
        r1 = Resistor(orient="v", name="R1", value="10 kΩ", p=(100, 100))
        r2 = Resistor(orient="v", name="R2", value="10 kΩ", p=(100, 300))
        rl = Resistor(orient="v", name="RL", value="10 kΩ", n=(400, 200))
        rs = Resistor(orient="h", name="RS", value="100 Ω", n=(500, 200))
        q2 = BJT(kind="npn", orient="v", name="Q2", value="BC547", c=(400, 200))
        t1 = BJT(kind="npn", orient="v", name="T1", value="SFH309 FA", c=(600, 300))
        # The SFH309 FA has no base lead; hide the generic BJT's base stub.
        schematic.add(
            schematic.rect(
                insert=(495, 340),
                size=(55, 20),
                stroke="none",
                fill="white",
            )
        )
        c1 = Capacitor(orient="v", name="C1", value="1 nF (optional)", p=(800, 200))
        stm32 = Box(
            i=(900, 200),
            name="STM32F103C8T6",
            value="PA1 (ADC1_IN1)",
            w=5,
            h=2,
            nudge=14,
            background="lightgray",
        )

        # Supply and divider. The divider midpoint biases Q2's base.
        Wire([(100, 100), (400, 100)])
        Wire([(100, 200), (100, 300)])
        Wire([(100, 250), q2.b])
        Wire([(100, 400), (100, 450), (800, 450)])

        # Cascode path: Q2 emitter drives T1's collector; T1 emitter returns to ground.
        Wire([q2.e, t1.c])
        Wire([t1.e, (600, 450)])

        # Q2 collector load and ADC interface.
        Wire([q2.c, rs.n])
        Wire([rs.p, c1.p, stm32.i])
        Wire([c1.n, (800, 450)])

        # Mark the supply, bias, and shared ground nodes.
        Dot(C=(100, 250))
        Dot(C=q2.c)
        Dot(C=rs.p)
        add_label(schematic, "PA1 / ADC1_IN1", (700, 180))
        add_label(schematic, "3V3", (160, 90))
        add_label(schematic, "V_bias ≈ 1.65 V", (190, 230))

        # The library has a BJT symbol; the arrows identify T1 as a photosensor.
        add_light_arrow(schematic, (535, 270), (555, 290))
        add_light_arrow(schematic, (535, 292), (555, 312))
        Ground(t=(400, 450))


if __name__ == "__main__":
    main()
