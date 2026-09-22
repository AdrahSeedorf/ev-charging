#!/usr/bin/env python3
"""Build the Hume heavy-vehicle rest-area network: stage 4's second domain.

Two stages, because only the first needs a file this repository does not ship.

  extract  nra.csv -> data/hume-trucks/rest_areas.csv
           Reads the National Freight Data Hub's "National Formal Rest Areas"
           file (downloaded by hand; see PROVENANCE.md), keeps every Hume row,
           and records for each whether it is used, why or why not, how many
           truck bays it is assumed to have, and on what basis.

  build    rest_areas.csv + data/hume/{nodes,edges}.csv -> data/hume-trucks/
           Places every included rest area on the corridor and writes the
           network, the demands and domain.txt. Deterministic: CI rebuilds it
           and fails if the committed files differ.

Run:  python3 tools/build_truck_network.py extract --nra ~/Downloads/nra.csv
      python3 tools/build_truck_network.py build

Scope, stated once: trucks travel Sydney -> Melbourne only, so only rest areas
serving that direction are used. The engine's stations have no direction, and
a southbound bay is no use to a northbound truck; modelling both directions
would need direction-aware stations, which is future work, not a detail.
"""

from __future__ import annotations

import argparse
import csv
import math
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HUME = ROOT / "data" / "hume"
OUT = ROOT / "data" / "hume-trucks"
REST_AREAS = OUT / "rest_areas.csv"

# ---------------------------------------------------------------------------
# Which rows serve a Sydney -> Melbourne truck
# ---------------------------------------------------------------------------

# NSW labels the Hume by its general bearing -- "Westbound" is away from Sydney
# -- and Victoria by compass direction. Paired sites on opposite carriageways
# confirm it: a "...Truck Parking Bay Southbound" is recorded as Westbound.
SYDNEY_TO_MELBOURNE = {
    "NSW": {"southbound", "westbound", "both directions"},
    "VIC": {"southbound", "both directions"},
}

# Truck bays assumed per site type. The dataset records no capacity at all, so
# this is the one estimated input to the whole study; every figure the study
# reports is therefore swept across a scale on these numbers rather than
# trusted at face value.
#
# Where a NSW site carries an Austroads class, the bays are that class's
# MINIMUM from Austroads AP-R591-19, "Guidelines for the Provision of Heavy
# Vehicle Rest Area Facilities", Table 4.1: Class 1-2 "20+", Class 3-4
# "10-15", Class 5 "5+". Everything else is an estimate, said so.
AUSTROADS = "Austroads AP-R591-19 Table 4.1 minimum"
BAYS_BY_TYPE: dict[str, tuple[int, str]] = {
    "class 1 heavy vehicle rest area": (20, AUSTROADS),
    "class 2 heavy vehicle rest area": (20, AUSTROADS),
    "class 3 heavy vehicle rest area": (10, AUSTROADS),
    "class 4 heavy vehicle rest area": (10, AUSTROADS),
    "class 5 heavy vehicle rest area": (5, AUSTROADS),
    "truck parking bay": (5, "estimate: a designed truck bay, sized as Austroads Class 5"),
    "parking area heavy rest areas": (5, "estimate: every such site is named a Truck Parking Bay; sized as Class 5"),
    "other rest area": (5, "estimate: type unstated, heavy vehicles flagged; sized as the smallest designed class"),
    "service centre": (20, "estimate: commercial roadhouse, sized as Austroads Class 1-2"),
    "cars and trucks": (10, "estimate: Victoria has no classes; a general rest area sized as Class 3-4"),
    "informal heavy rest areas": (2, "estimate: an informal pull-off, no design minimum exists"),
    "green reflector sites": (2, "estimate: a roadside truck stopping point, no design minimum exists"),
}

# Types that say outright a truck cannot rest there. The type decides wherever
# it names the vehicles a site takes; the heavy_vehicle_area flag decides only
# where the type is silent -- because the flag is demonstrably unreliable: nine
# "Light Vehicle Only Rest Area" rows on the Hume are flagged Y.
EXCLUDED_TYPES = {
    "light vehicle only rest area": "type says light vehicles only (whatever the flag says)",
    "informal light rest areas": "type says light vehicles only (whatever the flag says)",
    "cars only": "type says cars only",
    "weighbridge": "a weighbridge, not a place to rest",
}
TYPES_THAT_NAME_TRUCKS = {
    t for t in BAYS_BY_TYPE if t not in {"service centre", "other rest area"}
}

# The corridor is drawn as straight chords between towns up to 115 km apart,
# and the real Hume curves as much as ~16 km away from them -- Coolac, Mungi
# Mungi Hill and Conroys Gap are genuine Hume rest areas that sit that far off
# a chord. A first cut at 10 km silently dropped eight such sites, about 15% of
# the assumed bays. 25 km keeps every real site and would still reject a row
# whose coordinates put it on another road entirely.
MAX_OFFSET_KM = 25.0

# ---------------------------------------------------------------------------
# Demand: a scenario, not an observation
# ---------------------------------------------------------------------------

SEED = 20260921
FLEET = 300                # trucks per day, Sydney -> Melbourne; a scenario parameter
MAX_HOURS_ALREADY_WORKED = 9.0
MAX_WORK_HOURS = 12.0      # HVNL standard hours, solo driver


def extract(nra: Path) -> None:
    with nra.open(newline="", encoding="utf-8-sig") as f:
        rows = [r for r in csv.DictReader(f) if "hume" in (r["road_name"] or "").lower()]
    if not rows:
        sys.exit(f"error: no Hume rows in {nra} -- is this the National Formal Rest Areas file?")

    out = []
    for r in rows:
        kind = r["provider_type"].strip().lower()
        direction = (r["direction_of_travel"] or "").strip().lower()
        state = r["state"].strip()
        decision, reason, bays, basis = "N", "", 0, ""

        if "old hume" in r["road_name"].lower():
            reason = "on the bypassed Old Hume Highway"
        elif kind in EXCLUDED_TYPES:
            reason = EXCLUDED_TYPES[kind]
        elif kind not in BAYS_BY_TYPE:
            reason = f"unrecognised site type '{r['provider_type'].strip()}'"
        elif kind not in TYPES_THAT_NAME_TRUCKS and r["heavy_vehicle_area"].strip().upper() != "Y":
            reason = "type is silent about trucks and the heavy-vehicle flag is N"
        elif not direction:
            reason = "no direction of travel recorded"
        elif direction not in SYDNEY_TO_MELBOURNE.get(state, set()):
            reason = f"serves the other direction ({r['direction_of_travel'].strip()})"
        else:
            decision = "Y"
            bays, basis = BAYS_BY_TYPE[kind]

        out.append({
            "source_id": r["id"],
            "name": r["name"].strip() or f"{r['provider_type'].strip()} near {r['locality'].strip().title()}",
            "state": state,
            "road_name": r["road_name"].strip(),
            "locality": r["locality"].strip(),
            "direction_of_travel": r["direction_of_travel"].strip(),
            "site_type": r["provider_type"].strip(),
            "heavy_vehicle_area": r["heavy_vehicle_area"].strip(),
            "latitude": f"{float(r['latitude']):.6f}",
            "longitude": f"{float(r['longitude']):.6f}",
            "included": decision,
            "reason_excluded": reason,
            "assumed_bays": bays,
            "bays_basis": basis,
        })

    out.sort(key=lambda r: int(r["source_id"]))
    OUT.mkdir(parents=True, exist_ok=True)
    with REST_AREAS.open("w", newline="") as f:
        f.write("# Hume rows of the National Freight Data Hub's National Formal Rest Areas file,\n"
                "# with this study's decision on each. Derived subset; see PROVENANCE.md.\n")
        writer = csv.DictWriter(f, fieldnames=list(out[0].keys()), lineterminator="\n")
        writer.writeheader()
        writer.writerows(out)
    used = sum(r["included"] == "Y" for r in out)
    print(f"wrote {REST_AREAS.relative_to(ROOT)}: {len(out)} Hume rows, {used} used")


# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------

def read_csv(path: Path) -> list[dict]:
    with path.open(newline="") as f:
        return list(csv.DictReader(line for line in f if not line.startswith("#")))


def project(lat: float, lon: float, lat0: float) -> tuple[float, float]:
    """Local equirectangular km -- ample at corridor scale, where the question is
    only which leg a site is on and how far along it."""
    return lon * 111.320 * math.cos(math.radians(lat0)), lat * 110.574


def build() -> None:
    towns = read_csv(HUME / "nodes.csv")
    legs = {(int(e["from_id"]), int(e["to_id"])): float(e["distance_km"]) for e in read_csv(HUME / "edges.csv")}
    lat0 = sum(float(t["latitude"]) for t in towns) / len(towns)
    pts = [project(float(t["latitude"]), float(t["longitude"]), lat0) for t in towns]
    # Road distance from Sydney to each town, along the corridor.
    chain = [0.0]
    for i in range(len(towns) - 1):
        chain.append(chain[-1] + legs[(i, i + 1)])

    places = [{"name": t["name"], "chainage": chain[i], "bays": 0,
               "lat": float(t["latitude"]), "lon": float(t["longitude"])} for i, t in enumerate(towns)]

    dropped = []
    offsets: list[tuple[float, str]] = []
    for r in read_csv(REST_AREAS):
        if r["included"] != "Y":
            continue
        p = project(float(r["latitude"]), float(r["longitude"]), lat0)
        best = None
        for i in range(len(pts) - 1):
            (ax, ay), (bx, by) = pts[i], pts[i + 1]
            dx, dy = bx - ax, by - ay
            t = max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - ay) * dy) / (dx * dx + dy * dy)))
            off = math.hypot(p[0] - (ax + t * dx), p[1] - (ay + t * dy))
            if best is None or off < best[0]:
                best = (off, i, t)
        off, i, t = best
        if off > MAX_OFFSET_KM:
            dropped.append((r["name"], off))
            continue
        offsets.append((off, r["name"]))
        a, b = towns[i], towns[i + 1]
        places.append({
            "name": r["name"],
            # Along-leg position scaled from straight-line fraction to road km.
            "chainage": chain[i] + t * legs[(i, i + 1)],
            "bays": int(r["assumed_bays"]),
            # Placed ON the corridor, so the network's own geometric check --
            # no edge may be shorter than the straight line it spans -- holds.
            "lat": float(a["latitude"]) + t * (float(b["latitude"]) - float(a["latitude"])),
            "lon": float(a["longitude"]) + t * (float(b["longitude"]) - float(a["longitude"])),
        })

    places.sort(key=lambda p: (p["chainage"], p["bays"]))
    # Sites within 100 m along the corridor are one stopping place: merge them,
    # summing bays, rather than join them with a zero-length edge.
    merged: list[dict] = []
    for p in places:
        if merged and p["chainage"] - merged[-1]["chainage"] < 0.1 and (p["bays"] or merged[-1]["bays"]):
            m = merged[-1]
            m["name"] = m["name"] if m["bays"] and not p["bays"] else (p["name"] if not m["bays"] else f"{m['name']} + {p['name']}")
            m["bays"] += p["bays"]
            continue
        merged.append(dict(p))

    OUT.mkdir(parents=True, exist_ok=True)
    with (OUT / "nodes.csv").open("w", newline="") as f:
        f.write("# Hume corridor towns and the Sydney->Melbourne heavy-vehicle rest areas between\n"
                "# them. Bays are ASSUMED, not recorded -- see rest_areas.csv and PROVENANCE.md.\n")
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["id", "name", "has_station", "price_per_unit", "servers", "rate_per_hour", "latitude", "longitude"])
        for i, p in enumerate(merged):
            w.writerow([i, p["name"].replace(",", " "), 1 if p["bays"] else 0, "0.00", p["bays"], "0.0",
                        f"{p['lat']:.6f}", f"{p['lon']:.6f}"])
    with (OUT / "edges.csv").open("w", newline="") as f:
        f.write("# Road km between consecutive places, from the Hume leg distances; rest areas are\n"
                "# placed along each leg in proportion to their projected position.\n")
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["from_id", "to_id", "distance_km"])
        # Round POSITIONS, then difference them. Rounding each edge on its own let
        # 51 half-metre errors add up to a corridor 891.001 km long; differences
        # of rounded positions telescope, so the edges sum to the corridor exactly.
        at = [round(p["chainage"], 3) for p in merged]
        for i in range(len(merged) - 1):
            w.writerow([i, i + 1, f"{at[i + 1] - at[i]:.3f}"])

    rng = random.Random(SEED)
    last = len(merged) - 1
    with (OUT / "demands.csv").open("w", newline="") as f:
        f.write(f"# {FLEET} trucks, Sydney -> Melbourne, released uniformly over 24h, each having already\n"
                f"# worked 0-{MAX_HOURS_ALREADY_WORKED:g} of its {MAX_WORK_HOURS:g} hours. A SCENARIO, not observed traffic. Seed {SEED}.\n")
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["id", "origin_id", "destination_id", "capacity", "level", "consumption", "required_amount", "release_hour"])
        for k in range(FLEET):
            worked = rng.uniform(0.0, MAX_HOURS_ALREADY_WORKED)
            w.writerow([k + 1, 0, last, f"{MAX_WORK_HOURS:.1f}", f"{MAX_WORK_HOURS - worked:.3f}", "1.0", "0.0",
                        f"{rng.uniform(0.0, 24.0):.3f}"])

    (OUT / "domain.txt").write_text("truck\n")

    offsets.sort(reverse=True)
    stations = [p for p in merged if p["bays"]]
    print(f"wrote {OUT.relative_to(ROOT)}: {len(merged)} places ({len(stations)} rest areas, "
          f"{sum(p['bays'] for p in stations)} assumed bays) over {merged[-1]['chainage']:.0f} km; {FLEET} trucks")
    if offsets:
        print(f"  largest distance from the town-to-town chord: {offsets[0][0]:.1f} km ({offsets[0][1]})")
    for name, off in dropped:
        print(f"  dropped '{name}': {off:.1f} km off the corridor")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="stage", required=True)
    e = sub.add_parser("extract", help="nra.csv -> rest_areas.csv (needs the downloaded file)")
    e.add_argument("--nra", type=Path, required=True, help="path to the downloaded nra.csv")
    sub.add_parser("build", help="rest_areas.csv -> network, demands, domain.txt (reproducible)")
    args = parser.parse_args()
    if args.stage == "extract":
        extract(args.nra)
    else:
        build()


if __name__ == "__main__":
    main()
