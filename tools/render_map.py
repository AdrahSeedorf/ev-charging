#!/usr/bin/env python3
"""Draw the charging network as a map.

Produces two standalone SVGs (light and dark) for embedding in documentation, and an
interactive HTML page with hover detail and a table view.

Encoding decisions, since they are choices rather than defaults:

  * Station load is a MAGNITUDE, so it gets a sequential single-hue ramp -- blue,
    stepped so that distance from the surface reads as "more". The light and dark
    ramps are separate selected step sets, not an automatic inversion: on a dark
    surface the ramp has to run dark-to-bright for the same meaning to survive.
  * Station versus plain waypoint is IDENTITY, and it is encoded by shape -- filled
    disc against hollow ring -- rather than by a second hue. That keeps colour
    doing exactly one job.
  * Charger count is a second magnitude, encoded as radius.
  * A recommended new site is a STATUS, drawn in status-good with a text label
    beside it, so the colour never carries the meaning alone.

Usage:
    python3 tools/render_map.py --network data/sydney --out docs
    python3 tools/render_map.py --network data/sydney --load out/load.csv \\
        --site Leppington --title "Sydney metro: charger load" --out docs
"""

from __future__ import annotations

import argparse
import csv
import io
import math
from pathlib import Path

# --------------------------------------------------------------------------
# Palette. Validated with the dataviz skill's checker: both ramps pass the
# ordinal gates (single hue, monotone lightness, visible step gaps, light end
# clearing the surface) against their own surface.
# --------------------------------------------------------------------------

THEMES = {
    "light": {
        "surface": "#fcfcfb",
        "plane": "#f9f9f7",
        "ink": "#0b0b0b",
        "ink_secondary": "#52514e",
        "ink_muted": "#898781",
        "edge": "#c3c2b7",
        "hairline": "#e1e0d9",
        # low -> high load
        "ramp": ["#86b6ef", "#5598e7", "#2a78d6", "#1c5cab", "#104281"],
        "site": "#0ca30c",
    },
    "dark": {
        "surface": "#1a1a19",
        "plane": "#0d0d0d",
        "ink": "#ffffff",
        "ink_secondary": "#c3c2b7",
        "ink_muted": "#898781",
        "edge": "#383835",
        "hairline": "#2c2c2a",
        # On a dark surface the ramp runs dark -> bright so that magnitude still
        # reads as distance from the surface.
        "ramp": ["#184f95", "#256abf", "#3987e5", "#6da7ec", "#9ec5f4"],
        "site": "#0ca30c",
    },
}

BUCKET_LABELS = ["0-20%", "20-40%", "40-60%", "60-80%", "80-100%"]

WIDTH, HEIGHT = 1000, 720
PAD_LEFT, PAD_RIGHT, PAD_TOP, PAD_BOTTOM = 60, 210, 92, 56


# --------------------------------------------------------------------------
# Input
# --------------------------------------------------------------------------


def read_csv(path: Path) -> list[dict]:
    text = "".join(line for line in path.read_text().splitlines(keepends=True)
                   if not line.lstrip().startswith("#"))
    return list(csv.DictReader(io.StringIO(text)))


def load_network(directory: Path):
    nodes = read_csv(directory / "nodes.csv")
    edges = read_csv(directory / "edges.csv")
    for node in nodes:
        if not node.get("latitude") or not node.get("longitude"):
            raise SystemExit(f"error: '{node['name']}' has no coordinates -- "
                             f"run tools/build_datasets.py to regenerate")
        node["lat"] = float(node["latitude"])
        node["lon"] = float(node["longitude"])
        node["has_station"] = node["has_station"] == "1"
        node["chargers"] = int(node["chargers"])
        node["price"] = float(node["price_per_kwh"])
    return nodes, edges


def load_utilisation(path: Path) -> dict[str, dict]:
    """Peak and mean charger utilisation per station, from a --timeseries export."""
    samples: dict[str, list[float]] = {}
    for row in read_csv(path):
        samples.setdefault(row["station"], []).append(float(row["utilisation"]))
    return {
        name: {"peak": max(values), "mean": sum(values) / len(values)}
        for name, values in samples.items() if values
    }


# --------------------------------------------------------------------------
# Projection
# --------------------------------------------------------------------------


def project(nodes: list[dict]):
    """Equirectangular, with longitude scaled by cos(mean latitude).

    Adequate and honest at city and corridor scale: over a few hundred kilometres
    the distortion is well under the error already carried by the coordinates
    themselves. A national-scale map would want a proper projection.
    """
    lats = [n["lat"] for n in nodes]
    lons = [n["lon"] for n in nodes]
    scale_lon = math.cos(math.radians(sum(lats) / len(lats)))

    xs = [lon * scale_lon for lon in lons]
    ys = [-lat for lat in lats]  # negate so north is up
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)

    plot_w = WIDTH - PAD_LEFT - PAD_RIGHT
    plot_h = HEIGHT - PAD_TOP - PAD_BOTTOM
    # One scale for both axes, so the map keeps its shape.
    span = max(x1 - x0, 1e-9), max(y1 - y0, 1e-9)
    k = min(plot_w / span[0], plot_h / span[1])
    off_x = PAD_LEFT + (plot_w - span[0] * k) / 2
    off_y = PAD_TOP + (plot_h - span[1] * k) / 2

    for node, x, y in zip(nodes, xs, ys):
        node["x"] = off_x + (x - x0) * k
        node["y"] = off_y + (y - y0) * k
    return nodes


#: The legend occupies the right-hand column. Labels must not stray into it.
LEGEND_LEFT = WIDTH - PAD_RIGHT + 12


def place_labels(nodes: list[dict]) -> None:
    """Greedy label placement: try each offset in turn, take the first that neither
    collides with an already-placed label nor escapes the plot area.

    Crude, but it beats overlapping text -- and the plot-area clamp matters: without
    it the easternmost place (Manly, in the Sydney set) put its label straight through
    the legend.
    """
    placed: list[tuple[float, float, float, float]] = []
    offsets = [(11, 4, "start"), (-11, 4, "end"), (0, -12, "middle"), (0, 17, "middle"),
               (11, -8, "start"), (-11, -8, "end"), (11, 15, "start"), (-11, 15, "end")]

    def fits(box) -> bool:
        if box[0] < 6 or box[2] > LEGEND_LEFT:
            return False
        if box[1] < PAD_TOP - 24 or box[3] > HEIGHT - PAD_BOTTOM + 20:
            return False
        return not any(box[0] < q[2] and q[0] < box[2] and box[1] < q[3] and q[1] < box[3]
                       for q in placed)

    # Stations first: they carry the data, so they get the better positions.
    for node in sorted(nodes, key=lambda n: (not n["has_station"], n["name"])):
        width = 7.0 * len(node["name"])
        for dx, dy, anchor in offsets:
            x = node["x"] + dx
            y = node["y"] + dy
            left = x if anchor == "start" else (x - width if anchor == "end" else x - width / 2)
            box = (left, y - 9, left + width, y + 3)
            if fits(box):
                node["label"] = (x, y, anchor)
                placed.append(box)
                break
        else:
            # Nothing fit. Fall back to the side with more room, and accept overlap
            # rather than dropping the name.
            anchor = "end" if node["x"] > WIDTH / 2 else "start"
            node["label"] = (node["x"] + (-11 if anchor == "end" else 11), node["y"] + 4, anchor)


# --------------------------------------------------------------------------
# Drawing
# --------------------------------------------------------------------------


def bucket(value: float | None) -> int | None:
    if value is None:
        return None
    return min(4, max(0, int(value * 5.0 - 1e-9)))


def radius(chargers: int) -> float:
    # Area roughly proportional to capacity, floored so the smallest is still a
    # comfortable hit target.
    return 5.0 + 2.1 * math.sqrt(max(chargers, 0))


def esc(text: str) -> str:
    return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
                .replace('"', "&quot;"))


def draw(nodes, edges, theme_name: str, util: dict, site: str | None,
         title: str, subtitle: str, interactive: bool) -> str:
    t = THEMES[theme_name]
    out: list[str] = []
    a = out.append

    a(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {HEIGHT}" '
      f'width="{WIDTH}" height="{HEIGHT}" role="img" '
      f'aria-label="{esc(title)}. {esc(subtitle)}" '
      f'font-family="system-ui, -apple-system, Segoe UI, sans-serif">')
    a(f'<rect width="{WIDTH}" height="{HEIGHT}" fill="{t["surface"]}"/>')

    a(f'<text x="{PAD_LEFT}" y="40" fill="{t["ink"]}" font-size="21" '
      f'font-weight="600">{esc(title)}</text>')
    a(f'<text x="{PAD_LEFT}" y="63" fill="{t["ink_secondary"]}" font-size="13">'
      f'{esc(subtitle)}</text>')

    by_id = {n["id"]: n for n in nodes}

    # Roads first, so they sit behind the places they join. 2px, recessive ink.
    a(f'<g stroke="{t["edge"]}" stroke-width="2" stroke-linecap="round" fill="none" '
      f'opacity="0.85">')
    for edge in edges:
        p, q = by_id[edge["from_id"]], by_id[edge["to_id"]]
        a(f'<line x1="{p["x"]:.1f}" y1="{p["y"]:.1f}" '
          f'x2="{q["x"]:.1f}" y2="{q["y"]:.1f}"/>')
    a("</g>")

    # Places.
    for node in nodes:
        r = radius(node["chargers"]) if node["has_station"] else 5.0
        stats = util.get(node["name"])
        peak = stats["peak"] if stats else None
        idx = bucket(peak)
        name = node["name"]
        is_site = site is not None and name == site

        tip = [name]
        if node["has_station"]:
            price = "free" if node["price"] == 0 else f"${node['price']:.2f}/kWh"
            tip.append(f"{node['chargers']} chargers, {price}")
            if stats:
                tip.append(f"peak load {peak * 100:.0f}%, mean {stats['mean'] * 100:.0f}%")
            else:
                tip.append("no load data")
        else:
            tip.append("no charging station")
        if is_site:
            tip.append("recommended new site")

        group_attrs = ""
        if interactive:
            group_attrs = (f' class="node" tabindex="0" data-tip="{esc(" — ".join(tip))}"'
                           f' data-name="{esc(name)}"')
        a(f"<g{group_attrs}>")
        if not interactive:
            a(f"<title>{esc(' — '.join(tip))}</title>")

        if node["has_station"]:
            # 2px surface ring so overlapping discs stay legible.
            fill = t["ramp"][idx] if idx is not None else t["hairline"]
            a(f'<circle cx="{node["x"]:.1f}" cy="{node["y"]:.1f}" r="{r:.1f}" '
              f'fill="{fill}" stroke="{t["surface"]}" stroke-width="2"/>')
            if idx is None:
                a(f'<circle cx="{node["x"]:.1f}" cy="{node["y"]:.1f}" r="{r:.1f}" '
                  f'fill="none" stroke="{t["ink_muted"]}" stroke-width="1.5"/>')
        else:
            # Hollow ring: identity by shape, not by an extra hue.
            a(f'<circle cx="{node["x"]:.1f}" cy="{node["y"]:.1f}" r="{r:.1f}" '
              f'fill="{t["surface"]}" stroke="{t["ink_muted"]}" stroke-width="2"/>')

        if is_site:
            a(f'<circle cx="{node["x"]:.1f}" cy="{node["y"]:.1f}" r="{r + 5:.1f}" '
              f'fill="none" stroke="{t["site"]}" stroke-width="2.5"/>')

        lx, ly, anchor = node["label"]
        weight = "600" if node["has_station"] else "400"
        colour = t["ink"] if node["has_station"] else t["ink_muted"]
        a(f'<text x="{lx:.1f}" y="{ly:.1f}" text-anchor="{anchor}" fill="{colour}" '
          f'font-size="11" font-weight="{weight}">{esc(name)}</text>')
        if is_site:
            a(f'<text x="{lx:.1f}" y="{ly + 12:.1f}" text-anchor="{anchor}" '
              f'fill="{t["site"]}" font-size="10" font-weight="600">'
              f'&#9679; recommended site</text>')
        a("</g>")

    # Legend. Present whenever more than one thing is encoded, which is always here.
    lx = WIDTH - PAD_RIGHT + 24
    ly = PAD_TOP + 6
    a(f'<text x="{lx}" y="{ly}" fill="{t["ink_secondary"]}" font-size="11" '
      f'font-weight="600" letter-spacing="0.4">PEAK CHARGER LOAD</text>')
    for i, label in enumerate(BUCKET_LABELS):
        y = ly + 20 + i * 21
        a(f'<rect x="{lx}" y="{y - 9}" width="14" height="12" rx="3" '
          f'fill="{t["ramp"][i]}"/>')
        a(f'<text x="{lx + 22}" y="{y}" fill="{t["ink_secondary"]}" font-size="11">'
          f'{label}</text>')

    ly2 = ly + 20 + len(BUCKET_LABELS) * 21 + 18
    a(f'<text x="{lx}" y="{ly2}" fill="{t["ink_secondary"]}" font-size="11" '
      f'font-weight="600" letter-spacing="0.4">PLACE</text>')
    a(f'<circle cx="{lx + 7}" cy="{ly2 + 18}" r="7" fill="{t["ramp"][2]}" '
      f'stroke="{t["surface"]}" stroke-width="2"/>')
    a(f'<text x="{lx + 22}" y="{ly2 + 22}" fill="{t["ink_secondary"]}" font-size="11">'
      f'station (area = chargers)</text>')
    a(f'<circle cx="{lx + 7}" cy="{ly2 + 40}" r="5" fill="{t["surface"]}" '
      f'stroke="{t["ink_muted"]}" stroke-width="2"/>')
    a(f'<text x="{lx + 22}" y="{ly2 + 44}" fill="{t["ink_secondary"]}" font-size="11">'
      f'no station</text>')
    if site:
        a(f'<circle cx="{lx + 7}" cy="{ly2 + 62}" r="7" fill="none" '
          f'stroke="{t["site"]}" stroke-width="2.5"/>')
        a(f'<text x="{lx + 22}" y="{ly2 + 66}" fill="{t["ink_secondary"]}" '
          f'font-size="11">recommended site</text>')

    a(f'<text x="{PAD_LEFT}" y="{HEIGHT - 20}" fill="{t["ink_muted"]}" font-size="10">'
      f'Equirectangular projection, longitude scaled by cos(mean latitude). '
      f'Positions are place centres, accurate to about a kilometre.</text>')
    a("</svg>")
    return "\n".join(out)


def build_html(nodes, edges, util, site, title, subtitle) -> str:
    light = draw(nodes, edges, "light", util, site, title, subtitle, interactive=True)
    dark = draw(nodes, edges, "dark", util, site, title, subtitle, interactive=True)

    rows = []
    for node in sorted(nodes, key=lambda n: n["name"]):
        stats = util.get(node["name"])
        if not node["has_station"]:
            station, chargers, price = "—", "—", "—"
        else:
            station = "yes"
            chargers = str(node["chargers"])
            price = "free" if node["price"] == 0 else f"${node['price']:.2f}"
        peak = f"{stats['peak'] * 100:.0f}%" if stats else "—"
        mean = f"{stats['mean'] * 100:.0f}%" if stats else "—"
        coords = f"{node['lat']:.4f}, {node['lon']:.4f}"
        cells = [
            f"<td>{esc(node['name'])}</td>",
            f"<td>{station}</td>",
            f'<td class="num">{chargers}</td>',
            f'<td class="num">{price}</td>',
            f'<td class="num">{peak}</td>',
            f'<td class="num">{mean}</td>',
            f'<td class="num">{coords}</td>',
        ]
        rows.append("<tr>" + "".join(cells) + "</tr>")

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{esc(title)}</title>
<style>
  :root {{ color-scheme: light dark; }}
  body {{
    margin: 0; padding: 28px;
    font: 14px/1.55 system-ui, -apple-system, "Segoe UI", sans-serif;
    background: #f9f9f7; color: #0b0b0b;
  }}
  .card {{ background: #fcfcfb; border: 1px solid rgba(11,11,11,0.10);
           border-radius: 12px; padding: 14px; max-width: 1040px; }}
  .dark-only {{ display: none; }}
  h1 {{ font-size: 17px; margin: 0 0 4px; }}
  p.lede {{ color: #52514e; margin: 0 0 18px; max-width: 70ch; }}
  table {{ border-collapse: collapse; margin-top: 18px; font-size: 13px; }}
  caption {{ text-align: left; color: #52514e; padding-bottom: 8px; }}
  th, td {{ padding: 5px 12px 5px 0; border-bottom: 1px solid #e1e0d9; text-align: left; }}
  th {{ color: #52514e; font-weight: 600; }}
  td.num {{ font-variant-numeric: tabular-nums; }}
  #tip {{
    position: fixed; pointer-events: none; opacity: 0;
    background: #0b0b0b; color: #fff; padding: 6px 9px; border-radius: 6px;
    font-size: 12px; transition: opacity .08s; max-width: 320px; z-index: 10;
  }}
  .node {{ cursor: default; }}
  .node:focus {{ outline: none; }}
  @media (prefers-color-scheme: dark) {{
    body {{ background: #0d0d0d; color: #fff; }}
    .card {{ background: #1a1a19; border-color: rgba(255,255,255,0.10); }}
    p.lede, th, caption {{ color: #c3c2b7; }}
    th, td {{ border-bottom-color: #2c2c2a; }}
    .light-only {{ display: none; }}
    .dark-only {{ display: block; }}
    #tip {{ background: #fcfcfb; color: #0b0b0b; }}
  }}
</style>
</head>
<body>
<h1>{esc(title)}</h1>
<p class="lede">{esc(subtitle)} Hover or focus a place for detail. The table below
carries the same figures, so nothing here depends on colour alone.</p>
<div class="card light-only">{light}</div>
<div class="card dark-only">{dark}</div>
<div id="tip" role="status" aria-live="polite"></div>

<table>
  <caption>Every place in the network, with the values the map encodes.</caption>
  <thead><tr><th>Place</th><th>Station</th><th>Chargers</th><th>$/kWh</th>
  <th>Peak load</th><th>Mean load</th><th>Coordinates</th></tr></thead>
  <tbody>
    {"".join(rows)}
  </tbody>
</table>

<script>
  const tip = document.getElementById('tip');
  const show = (event, text) => {{
    tip.textContent = text;
    tip.style.opacity = '1';
    const x = (event.clientX ?? 0) + 14, y = (event.clientY ?? 0) + 14;
    tip.style.left = Math.min(x, window.innerWidth - tip.offsetWidth - 12) + 'px';
    tip.style.top = Math.min(y, window.innerHeight - tip.offsetHeight - 12) + 'px';
  }};
  const hide = () => {{ tip.style.opacity = '0'; }};
  for (const node of document.querySelectorAll('.node')) {{
    const text = node.dataset.tip;
    node.addEventListener('mousemove', (e) => show(e, text));
    node.addEventListener('mouseleave', hide);
    node.addEventListener('focus', (e) => {{
      const box = node.getBoundingClientRect();
      show({{ clientX: box.left, clientY: box.bottom }}, text);
    }});
    node.addEventListener('blur', hide);
  }}
</script>
</body>
</html>
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--network", type=Path, required=True,
                        help="directory holding nodes.csv and edges.csv")
    parser.add_argument("--load", type=Path, default=None,
                        help="a --timeseries export, to colour stations by load")
    parser.add_argument("--site", default=None, help="highlight this place as a recommended site")
    parser.add_argument("--title", default=None)
    parser.add_argument("--subtitle", default=None)
    parser.add_argument("--out", type=Path, required=True, help="output directory")
    parser.add_argument("--name", default=None, help="output basename [network dir name]")
    args = parser.parse_args()

    nodes, edges = load_network(args.network)
    project(nodes)
    place_labels(nodes)
    util = load_utilisation(args.load) if args.load else {}

    stations = sum(1 for n in nodes if n["has_station"])
    name = args.name or args.network.name
    title = args.title or f"{name.title()} charging network"
    subtitle = args.subtitle or (
        f"{len(nodes)} places, {stations} with charging stations, {len(edges)} road links."
        + ("" if util else " No load data: run with --load to colour by utilisation."))

    args.out.mkdir(parents=True, exist_ok=True)
    for theme in ("light", "dark"):
        path = args.out / f"{name}-map-{theme}.svg"
        path.write_text(draw(nodes, edges, theme, util, args.site, title, subtitle,
                             interactive=False))
        print(f"  wrote {path}")

    html = args.out / f"{name}-map.html"
    html.write_text(build_html(nodes, edges, util, args.site, title, subtitle))
    print(f"  wrote {html}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
