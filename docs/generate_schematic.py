#!/usr/bin/env -S uv run --script
# /// script
# dependencies = ["svg-schematic==1.3"]
# ///
"""Generate the SFH309 FA cascode schematic as an SVG."""

from pathlib import Path

from svg_schematic import BJT, Box, Dot, Resistor, Schematic, Wire


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


def add_line(schematic, start, end):
    schematic.add(
        schematic.line(
            start=start,
            end=end,
            stroke="black",
            stroke_width=2,
            stroke_linecap="round",
        )
    )


def draw_phototransistor(schematic, center):
    x, y = center
    collector = (x + 50, y - 50)
    emitter = (x + 50, y + 50)
    schematic.add(
        schematic.polyline(
            [collector, (x + 50, y - 37.5), (x, y - 25)],
            fill="none",
            stroke="black",
            stroke_width=2,
            stroke_linecap="round",
            stroke_linejoin="round",
        )
    )
    add_line(schematic, (x, y - 37.5), (x, y + 37.5))
    schematic.add(
        schematic.polyline(
            [(x, y + 25), (x + 50, y + 37.5), emitter],
            fill="none",
            stroke="black",
            stroke_width=2,
            stroke_linecap="round",
            stroke_linejoin="round",
        )
    )
    schematic.add(
        schematic.polygon(
            [(x + 11, y + 21), (x + 39, y + 35), (x + 8, y + 33)],
            fill="black",
            stroke="none",
        )
    )
    add_light_arrow(schematic, (x - 70, y - 60), (x - 30, y - 30))
    add_light_arrow(schematic, (x - 70, y - 25), (x - 30, y + 5))
    add_label(schematic, "T1", (x + 75, y - 8))
    add_label(schematic, "SFH309 FA", (x + 102, y + 13))
    return collector, emitter


def add_power_symbol(schematic, point):
    x, y = point
    schematic.add(
        schematic.polygon(
            [(x, y - 58), (x - 10, y - 40), (x + 10, y - 40)],
            fill="black",
            stroke="none",
        )
    )
    add_label(schematic, "3V3", (x, y - 68))


def add_ground_symbol(schematic, point):
    x, y = point
    add_line(schematic, point, (x, y + 12))
    for bar_y, width in ((y + 12, 36), (y + 20, 24), (y + 28, 12)):
        add_line(schematic, (x - width / 2, bar_y), (x + width / 2, bar_y))
    add_label(schematic, "GND", (x + 48, y + 24))


def main():
    with Schematic(
        filename=str(OUTPUT),
        font_family="sans-serif",
        font_size=16,
        line_width=2,
        dot_radius=4,
        pad=45,
    ) as schematic:
        r1 = Resistor(orient="v", name="R1", value="10 kΩ", p=(100, 100))
        r2 = Resistor(orient="v", name="R2", value="10 kΩ", p=(100, 300))
        rl = Resistor(orient="v", name="RL", value="10 kΩ", n=(400, 200))
        q2 = BJT(kind="npn", orient="v", name="Q2", value="BC547", c=(400, 200))
        t1_collector, t1_emitter = draw_phototransistor(schematic, (350, 400))
        stm32 = Box(
            i=(550, 200),
            name="STM32F103C8T6",
            value="PA1 (ADC1_IN1)",
            w=5,
            h=2,
            nudge=14,
            background="lightgray",
        )

        Wire([(100, 100), (400, 100)])
        Wire([(100, 200), (100, 300)])
        Wire([(100, 250), q2.b])
        Wire([(100, 400), (100, 500), (400, 500)])
        Wire([(250, 100), (250, 60)])

        Wire([q2.e, t1_collector])
        Wire([t1_emitter, (t1_emitter[0], 500)])
        Wire([q2.c, stm32.i])

        Dot(C=(250, 100))
        Dot(C=(100, 250))
        Dot(C=q2.c)
        add_power_symbol(schematic, (250, 100))
        add_ground_symbol(schematic, (250, 500))



if __name__ == "__main__":
    main()
