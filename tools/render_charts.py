#!/usr/bin/env python3
"""Draw the project's findings as SVG charts, light and dark.

The README's findings were all tables. Several of them are comparisons or trends,
which is exactly what charts are for -- a ratio of 1,476x is a shrug in a table and
a shock in a chart.

Everything here follows the dataviz method: the form is chosen from the data's job
BEFORE any colour is picked, and every palette below was run through the skill's
validator rather than eyeballed. Recorded results, so a later edit can be checked
against them:

  categorical 1-3, all-pairs   light PASS (worst CVD dE 9.2)   dark PASS (9.4)
  categorical 1-5, adjacent    light PASS (worst CVD dE 9.1)   dark PASS (8.4)
  ordinal pair (dumbbell)      light PASS                      dark PASS

Three light-mode series sit under 3:1 against the light surface. That is a WARN,
not a pass, and it obliges relief: every series in this file is directly labelled,
never identified by colour alone.

GitHub renders static SVG in a README and strips scripts, so there is no hover
layer here -- the numbers a tooltip would show are printed on the marks or in the
README table beside the chart.

Usage:
    python3 tools/render_charts.py --out docs
"""

from __future__ import annotations

import argparse
import csv
import io
import math
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# --------------------------------------------------------------------------
# Palette. Identical roles to tools/render_map.py, so charts and maps read as
# one system. Values from the dataviz reference palette; see the header for the
# validator results.
# --------------------------------------------------------------------------

THEMES = {
    "light": {
        "surface": "#fcfcfb",
        "plane": "#f9f9f7",
        "ink": "#0b0b0b",
        "ink_secondary": "#52514e",
        "ink_muted": "#898781",
        "grid": "#e1e0d9",
        "axis": "#c3c2b7",
        "series": ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"],
        "context": "#c3c2b7",       # de-emphasised lines
        "ordinal_low": "#86b6ef",   # dumbbell: the measured end
        "ordinal_high": "#184f95",  # dumbbell: the modelled end
        "critical": "#d03b3b",
    },
    "dark": {
        "surface": "#1a1a19",
        "plane": "#0d0d0d",
        "ink": "#ffffff",
        "ink_secondary": "#c3c2b7",
        "ink_muted": "#898781",
        "grid": "#2c2c2a",
        "axis": "#383835",
        "series": ["#3987e5", "#d95926", "#199e70", "#c98500", "#d55181"],
        "context": "#383835",
        "ordinal_low": "#9ec5f4",
        "ordinal_high": "#184f95",
        "critical": "#d03b3b",
    },
}

PLANNERS = ["farthest", "cheapest", "min-wait", "generalised", "optimal"]


def esc(text: str) -> str:
    return (str(text).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


class Svg:
    """A tiny SVG builder: accumulate elements, then join."""

    def __init__(self, width: int, height: int, theme: dict, title: str, subtitle: str):
        self.w, self.h, self.t = width, height, theme
        self.parts: list[str] = []
        self.parts.append(
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
            f'width="{width}" height="{height}" role="img" '
            f'aria-label="{esc(title)}. {esc(subtitle)}" '
            f'font-family="ui-sans-serif, -apple-system, Segoe UI, Roboto, sans-serif">'
        )
        self.rect(0, 0, width, height, theme["surface"])
        self.text(28, 38, title, 17, theme["ink"], weight="700")
        if subtitle:
            self.text(28, 60, subtitle, 12.5, theme["ink_secondary"])

    def rect(self, x, y, w, h, fill, rx=0):
        self.parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
                          f'fill="{fill}" rx="{rx}"/>')

    def line(self, x1, y1, x2, y2, stroke, width=1.0, cap="butt", dash=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        self.parts.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
                          f'stroke="{stroke}" stroke-width="{width}" stroke-linecap="{cap}"{d}/>')

    def path(self, d, stroke, width=2.0, fill="none", opacity=1.0):
        self.parts.append(f'<path d="{d}" fill="{fill}" stroke="{stroke}" stroke-width="{width}" '
                          f'stroke-linejoin="round" stroke-linecap="round" opacity="{opacity}"/>')

    def circle(self, cx, cy, r, fill, stroke=None, width=2.0):
        s = f' stroke="{stroke}" stroke-width="{width}"' if stroke else ""
        self.parts.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r}" fill="{fill}"{s}/>')

    def text(self, x, y, body, size=12, fill=None, anchor="start", weight="400", opacity=1.0):
        fill = fill or self.t["ink"]
        self.parts.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" '
                          f'text-anchor="{anchor}" font-weight="{weight}" opacity="{opacity}">'
                          f'{esc(body)}</text>')

    def footer(self, body):
        self.text(28, self.h - 16, body, 11, self.t["ink_muted"])

    def done(self) -> str:
        return "\n".join(self.parts) + "\n</svg>\n"


# --------------------------------------------------------------------------
# Data
# --------------------------------------------------------------------------


def evnet() -> Path:
    for p in (ROOT / "build/debug/evnet", ROOT / "build/release/evnet", ROOT / "build/evnet"):
        if p.is_file():
            return p
    raise SystemExit("error: build the project first (cmake --build build/debug)")


def run_csv(args: list[str]) -> list[dict]:
    out = subprocess.run([str(evnet())] + args, capture_output=True, text=True, cwd=ROOT)
    if out.returncode != 0:
        raise SystemExit(f"error: evnet {' '.join(args)} failed:\n{out.stderr}")
    rows = [l for l in out.stdout.splitlines() if l and not l.startswith("#")]
    return list(csv.DictReader(io.StringIO("\n".join(rows))))


def read_csv(path: Path) -> list[dict]:
    text = "".join(l for l in path.read_text().splitlines(keepends=True)
                   if not l.lstrip().startswith("#"))
    return list(csv.DictReader(io.StringIO(text)))


# --------------------------------------------------------------------------
# Chart 1 -- station load over the day
#
# Job: trend over time, one series is the point and the rest are context. The
# dataviz form for that is EMPHASIS, not twelve categorical hues: three accent
# lines for the stations that saturate, everything else in the context grey.
# --------------------------------------------------------------------------


def chart_load(theme_name: str, series_path: Path) -> str:
    """Network occupancy through the day.

    The first version of this chart drew all twelve towns and highlighted the three
    with the highest PEAK utilisation. That was wrong twice over: nine of the twelve
    touch 100% at some instant, because a three-charger town goes 0-33-67-100 in
    single steps -- so peak separates nothing -- and the resulting twelve-line
    spaghetti had no message in it at all.

    What is true, and worth showing, is the network total: 48 chargers, never more
    than 18 in use at once and never more than 2 vehicles waiting. That is Finding
    1's uncomfortable corollary drawn rather than asserted -- at its inherited fleet
    size this corridor is barely congested, so the legacy project's queue-balancing
    was solving a problem its own data does not have.
    """
    t = THEMES[theme_name]
    rows = read_csv(series_path)

    charging: dict[float, int] = {}
    waiting: dict[float, int] = {}
    capacity = 0
    for r in rows:
        h = float(r["time_hours"])
        charging[h] = charging.get(h, 0) + int(r["charging"])
        waiting[h] = waiting.get(h, 0) + int(r["waiting"])
    for r in rows:
        if float(r["time_hours"]) == min(charging):
            capacity += int(r["chargers"])

    times = sorted(charging)
    peak_c, peak_w = max(charging.values()), max(waiting.values())

    W, H = 860, 430
    L, R, TOP, BOT = 62, 196, 108, 62
    svg = Svg(W, H, t, "The corridor through a simulated day",
              "Hume, 199 vehicles, generalised planner. Chargers occupied and "
              "vehicles queueing, against the network's capacity.")

    xmax = max(times) or 1.0
    plot_w, plot_h = W - L - R, H - TOP - BOT
    ymax = capacity

    def px(x): return L + (x / xmax) * plot_w
    def py(v): return TOP + (1 - v / ymax) * plot_h

    for v in range(0, ymax + 1, 12):
        y = py(v)
        svg.line(L, y, L + plot_w, y, t["grid"], 1)
        svg.text(L - 10, y + 4, str(v), 11, t["ink_muted"], anchor="end")
    for hour in range(0, int(xmax) + 1, 4):
        x = px(hour)
        svg.line(x, TOP, x, TOP + plot_h, t["grid"], 1)
        svg.text(x, TOP + plot_h + 20, f"{hour}h", 11, t["ink_muted"], anchor="middle")
    svg.line(L, TOP + plot_h, L + plot_w, TOP + plot_h, t["axis"], 1.5)

    # Capacity, as a reference the two series are read against.
    svg.line(L, py(capacity), L + plot_w, py(capacity), t["axis"], 1.5, dash="5 4")
    svg.text(L + plot_w - 6, py(capacity) - 8, f"{capacity} chargers on the corridor", 11,
             t["ink_secondary"], anchor="end")

    area = ("M " + f"{px(times[0]):.1f} {py(0):.1f} L "
            + " L ".join(f"{px(x):.1f} {py(charging[x]):.1f}" for x in times)
            + f" L {px(times[-1]):.1f} {py(0):.1f} Z")
    svg.parts.append(f'<path d="{area}" fill="{t["series"][0]}" opacity="0.14"/>')
    svg.path("M " + " L ".join(f"{px(x):.1f} {py(charging[x]):.1f}" for x in times),
             t["series"][0], 2.0)
    svg.path("M " + " L ".join(f"{px(x):.1f} {py(waiting[x]):.1f}" for x in times),
             t["series"][1], 2.0)

    lx = L + plot_w + 16
    svg.line(lx, TOP + 8, lx + 18, TOP + 8, t["series"][0], 2.5)
    svg.text(lx + 26, TOP + 12, "charging", 12, t["ink"])
    svg.text(lx, TOP + 30, f"peak {peak_c} of {capacity}", 11, t["ink_muted"])
    svg.line(lx, TOP + 58, lx + 18, TOP + 58, t["series"][1], 2.5)
    svg.text(lx + 26, TOP + 62, "waiting", 12, t["ink"])
    svg.text(lx, TOP + 80, f"peak {peak_w} vehicles", 11, t["ink_muted"])
    svg.text(lx, TOP + 118, f"{(1 - peak_c / capacity) * 100:.0f}% of the", 11.5,
             t["ink_secondary"])
    svg.text(lx, TOP + 134, "corridor is idle", 11.5, t["ink_secondary"])
    svg.text(lx, TOP + 150, "at its busiest.", 11.5, t["ink_secondary"])

    svg.text(28, TOP - 16, "vehicles", 11, t["ink_muted"])
    svg.footer("evnet simulate --network data/hume --timeseries load.csv")
    return svg.done()


# --------------------------------------------------------------------------
# Chart 2 -- what the static model claimed vs what the clock measured
#
# Job: before -> after per item. Dumbbell, one hue in two shades, on a LOG axis:
# the values span three orders of magnitude and a linear axis would flatten every
# measured value onto the baseline, hiding the very thing the chart is about.
# --------------------------------------------------------------------------


def chart_wait_error(theme_name: str, static_rows, event_rows) -> str:
    t = THEMES[theme_name]
    st = {r["planner"]: float(r["mean_wait_h"]) for r in static_rows}
    ev = {r["planner"]: float(r["mean_wait_h"]) for r in event_rows}
    names = [p for p in PLANNERS if p in st and p in ev]

    W, H = 860, 360
    L, R, TOP, BOT = 130, 130, 124, 62
    svg = Svg(W, H, t, "The static model's waits against the measured ones",
              "Hume corridor, same fleet, same planners — only the congestion "
              "model differs. Log scale; the gap is the error.")

    lo, hi = 0.001, 100.0
    plot_w, plot_h = W - L - R, H - TOP - BOT

    def px(v): return L + (math.log10(max(v, lo)) - math.log10(lo)) / \
        (math.log10(hi) - math.log10(lo)) * plot_w

    for decade in (0.001, 0.01, 0.1, 1, 10, 100):
        x = px(decade)
        svg.line(x, TOP - 8, x, TOP + plot_h, t["grid"], 1)
        label = f"{decade:g}h" if decade >= 1 else f"{decade}".rstrip("0").rstrip(".") + "h"
        svg.text(x, TOP + plot_h + 22, label, 11, t["ink_muted"], anchor="middle")

    row_h = plot_h / len(names)
    for i, name in enumerate(names):
        y = TOP + row_h * (i + 0.5)
        a, b = px(ev[name]), px(st[name])
        svg.text(L - 16, y + 4, name, 12.5, t["ink"], anchor="end")
        svg.line(a, y, b, y, t["axis"], 2.5, cap="round")
        # 2px surface ring so the two ends stay legible where they crowd.
        svg.circle(a, y, 6, t["ordinal_low"], t["surface"], 2)
        svg.circle(b, y, 6, t["ordinal_high"], t["surface"], 2)
        ratio = st[name] / ev[name] if ev[name] > 0 else float("inf")
        svg.text(b + 16, y + 4, f"{ratio:,.0f}× too high", 12, t["ink_secondary"])

    ly = TOP - 30
    svg.circle(L + 8, ly, 6, t["ordinal_low"], t["surface"], 2)
    svg.text(L + 22, ly + 4, "measured (event-driven clock)", 12, t["ink"])
    svg.circle(L + 250, ly, 6, t["ordinal_high"], t["surface"], 2)
    svg.text(L + 264, ly + 4, "claimed (static tally)", 12, t["ink"])

    svg.footer("evnet compare --engine static  vs  --engine events")
    return svg.done()


# --------------------------------------------------------------------------
# Chart 3 -- when the choice of planner starts to matter
#
# Job: tell distinct series apart over a trend. Five categorical hues, each
# directly labelled at its right end.
# --------------------------------------------------------------------------


def chart_saturation(theme_name: str, sweep_rows) -> str:
    t = THEMES[theme_name]
    fleets = sorted({int(r["fleet"]) for r in sweep_rows})
    cost = {p: {int(r["fleet"]): float(r["mean_generalised"])
                for r in sweep_rows if r["planner"] == p} for p in PLANNERS}

    W, H = 860, 430
    L, R, TOP, BOT = 72, 165, 92, 58
    svg = Svg(W, H, t, "When the choice of planner starts to matter",
              "Hume corridor as the fleet grows. Below saturation every strategy "
              "costs the same; past it they diverge.")

    lo = min(min(v.values()) for v in cost.values())
    hi = max(max(v.values()) for v in cost.values())
    lo, hi = 0, math.ceil(hi / 500) * 500
    plot_w, plot_h = W - L - R, H - TOP - BOT

    def px(i): return L + (i / (len(fleets) - 1)) * plot_w
    def py(v): return TOP + (1 - (v - lo) / (hi - lo)) * plot_h

    for step in range(0, hi + 1, 500):
        y = py(step)
        svg.line(L, y, L + plot_w, y, t["grid"], 1)
        svg.text(L - 10, y + 4, f"${step:,}", 11, t["ink_muted"], anchor="end")
    for i, f in enumerate(fleets):
        svg.text(px(i), TOP + plot_h + 20, f"{f:,}", 11, t["ink_muted"], anchor="middle")
    svg.line(L, TOP + plot_h, L + plot_w, TOP + plot_h, t["axis"], 1.5)
    svg.text(L + plot_w / 2, H - 16, "vehicles per day", 11, t["ink_muted"], anchor="middle")
    svg.text(28, TOP - 16, "mean generalised cost per trip", 11, t["ink_muted"])

    ends = []
    for slot, p in enumerate(PLANNERS):
        pts = [(px(i), py(cost[p][f])) for i, f in enumerate(fleets)]
        svg.path("M " + " L ".join(f"{x:.1f} {y:.1f}" for x, y in pts), t["series"][slot], 2.0)
        for x, y in pts:
            svg.circle(x, y, 4, t["series"][slot], t["surface"], 2)
        ends.append((pts[-1][1], p, slot))

    # Direct labels, nudged apart so they cannot overlap.
    ends.sort()
    last = -1e9
    for y, p, slot in ends:
        y = max(y, last + 18)
        last = y
        svg.line(L + plot_w + 10, y - 4, L + plot_w + 26, y - 4, t["series"][slot], 2.5)
        svg.text(L + plot_w + 34, y, p, 12, t["ink"])

    # The point of the chart, said in words on the chart.
    spread_lo = max(cost[p][fleets[0]] for p in PLANNERS) - min(cost[p][fleets[0]] for p in PLANNERS)
    spread_hi = max(cost[p][fleets[-1]] for p in PLANNERS) - min(cost[p][fleets[-1]] for p in PLANNERS)
    svg.text(px(0) + 6, py(cost["farthest"][fleets[0]]) - 18,
             f"spread ${spread_lo:,.0f}", 11.5, t["ink_secondary"])
    svg.text(px(len(fleets) - 1) - 6, py(cost["farthest"][fleets[-1]]) - 18,
             f"spread ${spread_hi:,.0f}", 11.5, t["ink_secondary"], anchor="end")

    svg.footer("python3 tools/saturation_sweep.py")
    return svg.done()


# --------------------------------------------------------------------------
# Chart 4 -- the two engines rank the candidate sites differently
#
# Job: before -> after per item, where the value IS the rank. A slope chart shows
# re-ordering better than two lists side by side, which is the entire claim.
# --------------------------------------------------------------------------


def parse_site_table(text: str) -> list[str]:
    """Site names in the order the command ranked them."""
    names = []
    for line in text.splitlines():
        if line.startswith("Site "):
            names.append(" ".join(line.split()[:2]))
    return names


def chart_siting(theme_name: str, static_names, event_names) -> str:
    t = THEMES[theme_name]
    shared = [n for n in event_names if n in static_names]
    ev_rank = {n: i + 1 for i, n in enumerate(event_names)}
    st_rank = {n: i + 1 for i, n in enumerate(static_names)}

    W, H = 860, 500
    TOP, BOT = 118, 82
    xl, xr = 300, 560
    svg = Svg(W, H, t, "The two engines disagree about where to build",
              "Same network, same fleet, same candidates — ranked by each engine. "
              "Crossing lines are changed recommendations.")

    n = len(shared)
    plot_h = H - TOP - BOT

    def py(rank): return TOP + (rank - 1) / max(n - 1, 1) * plot_h

    svg.text(xl, TOP - 26, "static tally", 12.5, t["ink"], anchor="end", weight="600")
    svg.text(xr, TOP - 26, "event-driven clock", 12.5, t["ink"], weight="600")

    movers = sorted(shared, key=lambda s: -abs(st_rank[s] - ev_rank[s]))[:2]
    for name in shared:
        y1, y2 = py(st_rank[name]), py(ev_rank[name])
        moved = name in movers
        colour = t["series"][0] if moved else t["context"]
        svg.path(f"M {xl} {y1:.1f} C {xl + 90} {y1:.1f}, {xr - 90} {y2:.1f}, {xr} {y2:.1f}",
                 colour, 2.5 if moved else 1.5, opacity=1.0 if moved else 0.75)
        ink = t["ink"] if moved else t["ink_muted"]
        svg.text(xl - 12, y1 + 4, f"{st_rank[name]}. {name}", 12, ink, anchor="end",
                 weight="600" if moved else "400")
        svg.text(xr + 12, y2 + 4, f"{ev_rank[name]}. {name}", 12, ink,
                 weight="600" if moved else "400")
        svg.circle(xl, y1, 4.5, colour, t["surface"], 2)
        svg.circle(xr, y2, 4.5, colour, t["surface"], 2)

    top_static, top_events = static_names[0], event_names[0]
    if top_static != top_events:
        svg.text(28, H - 34,
                 f"The static model would build at {top_static}. On the clock, "
                 f"{top_events} is better.", 12, t["ink"])
    svg.footer("evnet site --engine static  vs  --engine events")
    return svg.done()


# --------------------------------------------------------------------------
# Chart 5 -- the architecture, and where the domain seam is
#
# Not a chart: a diagram. It earns its place because the project's headline claim
# -- that the core is domain-agnostic -- is otherwise only prose.
# --------------------------------------------------------------------------


def chart_architecture(theme_name: str) -> str:
    t = THEMES[theme_name]
    W, H = 860, 530
    svg = Svg(W, H, t, "How a journey is decided",
              "Everything above the seam is about agents competing for capacity at "
              "nodes. Only the bottom band knows what a car is.")

    def box(x, y, w, h, label, detail, fill, ink, border=None):
        svg.rect(x, y, w, h, fill, rx=6)
        if border:
            svg.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" '
                             f'fill="none" stroke="{border}" stroke-width="1.5"/>')
        svg.text(x + w / 2, y + (22 if detail else h / 2 + 4), label, 12.5, ink,
                 anchor="middle", weight="600")
        if detail:
            svg.text(x + w / 2, y + 40, detail, 11, ink, anchor="middle", opacity=0.85)

    def arrow(x1, y1, x2, y2, label=None):
        svg.line(x1, y1, x2, y2, t["axis"], 1.5)
        svg.parts.append(
            f'<polygon points="{x2},{y2} {x2 - 6},{y2 - 5} {x2 - 6},{y2 + 5}" fill="{t["axis"]}"/>')
        if label:
            svg.text((x1 + x2) / 2, y1 - 8, label, 10.5, t["ink_muted"], anchor="middle")

    pale = t["plane"]
    y0 = 92
    box(28, y0, 150, 56, "Network", "nodes, edges", pale, t["ink"], t["axis"])
    arrow(178, y0 + 28, 220, y0 + 28)
    box(220, y0, 150, 56, "Router", "Dijkstra, cached", pale, t["ink"], t["axis"])
    arrow(370, y0 + 28, 412, y0 + 28)
    box(412, y0, 170, 56, "Candidates", "one feasibility rule", pale, t["ink"], t["axis"])

    y1 = y0 + 96
    svg.text(28, y1 + 18, "PLANNERS — what to do next", 11, t["ink_muted"], weight="600")
    box(28, y1 + 28, 250, 52, "Greedy × 4", "score only the next stop",
        pale, t["ink"], t["axis"])
    box(300, y1 + 28, 282, 52, "Optimal", "Dijkstra over (node, charge)",
        pale, t["ink"], t["series"][0])

    y2 = y1 + 108
    svg.text(28, y2 + 18, "ENGINES — what congestion means", 11, t["ink_muted"], weight="600")
    box(28, y2 + 28, 250, 52, "Static tally", "timeless; kept for comparison",
        pale, t["ink_secondary"], t["axis"])
    box(300, y2 + 28, 282, 52, "Event-driven clock", "waits are measured, not guessed",
        pale, t["ink"], t["series"][0])

    y3 = y2 + 100
    seam = y3 + 22
    svg.line(28, seam, W - 28, seam, t["series"][1], 2, dash="6 5")
    svg.text(W - 28, seam - 10, "the domain seam", 11.5, t["series"][1], anchor="end", weight="600")
    box(28, seam + 18, 554, 52, "EV specifics",
        "kWh ↔ km, charge curves, prices — the only layer that knows about cars",
        pale, t["ink_secondary"], t["series"][1])

    # Right-hand column: what comes out.
    box(612, y0, 220, 56, "route", "one vehicle's plan", pale, t["ink"], t["axis"])
    box(612, y1 + 28, 220, 52, "compare", "every planner, tabulated", pale, t["ink"], t["axis"])
    box(612, y2 + 28, 220, 52, "site", "where to build next", pale, t["ink"], t["axis"])
    svg.text(722, y0 - 18, "COMMANDS", 11, t["ink_muted"], anchor="middle", weight="600")

    svg.footer("Swap the bottom band and the engine above it is unchanged.")
    return svg.done()


# --------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / "docs")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    print("  gathering data")
    series_path = Path("/tmp/evnet-load.csv")
    subprocess.run([str(evnet()), "simulate", "--network", "data/hume",
                    "--demands", "data/hume/demands.csv", "--planner", "generalised",
                    "--timeseries", str(series_path)],
                   cwd=ROOT, capture_output=True, check=True)
    static_rows = run_csv(["compare", "--network", "data/hume",
                           "--demands", "data/hume/demands.csv",
                           "--engine", "static", "--format", "csv"])
    event_rows = run_csv(["compare", "--network", "data/hume",
                          "--demands", "data/hume/demands.csv",
                          "--engine", "events", "--format", "csv"])
    sweep_rows = read_csv(ROOT / "data" / "saturation-sweep.csv")

    net = "data/sydney-real"
    site_ev = subprocess.run([str(evnet()), "site", "--network", net, "--demands",
                              f"{net}/demands.csv", "--top", "0"],
                             cwd=ROOT, capture_output=True, text=True).stdout
    site_st = subprocess.run([str(evnet()), "site", "--network", net, "--demands",
                              f"{net}/demands.csv", "--top", "0", "--engine", "static"],
                             cwd=ROOT, capture_output=True, text=True).stdout

    charts = {
        "load-over-day": lambda th: chart_load(th, series_path),
        "wait-error": lambda th: chart_wait_error(th, static_rows, event_rows),
        "saturation": lambda th: chart_saturation(th, sweep_rows),
        "siting-disagreement": lambda th: chart_siting(
            th, parse_site_table(site_st), parse_site_table(site_ev)),
        "architecture": chart_architecture,
    }

    for name, build in charts.items():
        for theme in ("light", "dark"):
            path = args.out / f"{name}-{theme}.svg"
            path.write_text(build(theme))
            print(f"  wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
