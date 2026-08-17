#!/usr/bin/env python3
"""Convert the two legacy coursework datasets into the toolkit's unified schema.

This is the first inhabitant of the thin Python layer: data preparation and
validation, where Python earns its place. The C++ core never parses anything
more exotic than a well-formed CSV.

Two sources, one schema:

  * Sydney metro  -- data/sydney/raw/{Locations.txt,Weights.txt} from the
    legacy `ev_charging` project: 24 suburbs, arbitrary graph, a 24x24
    adjacency matrix, per-station prices, no charger counts.

  * Hume corridor -- the parallel `const` arrays formerly in `ev_charging1`'s
    Constant.h: 12 towns in a chain, real charger counts, no prices.

The output schema (nodes.csv / edges.csv / demands.csv) is deliberately an
EDGE LIST rather than an adjacency matrix. The legacy 24x24 matrix was ~90%
zeros, and a dense matrix does not survive contact with real OpenStreetMap
data in stage 3.

Run:  python3 tools/build_datasets.py
"""

from __future__ import annotations

import csv
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data"

# --------------------------------------------------------------------------
# Modelling constants
# --------------------------------------------------------------------------

# Energy consumption used to reconcile the two legacy unit systems. The Sydney
# project thought in kWh and dollars; the Hume project thought in km of battery
# range. This single factor lets them speak to each other -- it is the pivot the
# whole merge turns on. 18 kWh/100km is a reasonable mid-size EV figure.
EFFICIENCY_KWH_PER_100KM = 18.0


# --------------------------------------------------------------------------
# Departure times
# --------------------------------------------------------------------------

# Stage 2's event-driven engine needs to know WHEN each vehicle sets off. A fleet
# that all departs at t=0 is not a traffic pattern, it is a thundering herd, and it
# would make every station's queue peak in the first instant and then never again.
#
# This is a twin-peaked profile over a 16-hour day: a morning surge, a lighter
# afternoon one, and a uniform background. Crude compared with real traffic counts
# -- stage 3's job -- but it produces the overlapping arrivals that make congestion
# a process rather than a coincidence.
DAY_LENGTH_HOURS = 16.0


def departure_hour(rng: random.Random) -> float:
    roll = rng.random()
    if roll < 0.45:
        hour = rng.gauss(7.5, 1.5)      # morning peak
    elif roll < 0.75:
        hour = rng.gauss(14.0, 2.0)     # afternoon peak
    else:
        hour = rng.uniform(0.0, DAY_LENGTH_HOURS)
    return min(max(hour, 0.0), DAY_LENGTH_HOURS)

# --------------------------------------------------------------------------
# Sydney metro
# --------------------------------------------------------------------------

# Corrections applied to the legacy Weights.txt. See data/sydney/PROVENANCE.md
# for the full reasoning. Each entry is (node_a, node_b, distance_km) and each
# replaces a malformed or one-directional entry in the raw matrix.
SYDNEY_EDGE_CORRECTIONS = [
    # 'Macquarie Pk -> Campbelltown = 5.2' is geographically impossible
    # (~50 km apart). The value belongs one column later, as the mirror of
    # 'Ryde -> Macquarie Pk = 5.2' (~5 km), which is present and correct.
    ("Macquarie Pk", "Campbelltown", None),  # drop
    ("Macquarie Pk", "Ryde", 5.2),           # restore
    # 'Fairfield -> Leppington = 9.3' is the misplaced mirror of
    # 'Liverpool -> Fairfield = 9.3' (~9 km, correct).
    ("Fairfield", "Leppington", None),       # drop
    ("Liverpool", "Fairfield", 9.3),         # restore
    # 'Campbelltown -> Leppington = 14.8' is correct but has no reverse.
    ("Campbelltown", "Leppington", 14.8),    # make bidirectional
]

# Charger counts and power ratings are absent from the legacy Sydney data --
# that project never modelled congestion at all. These are plausible estimates
# scaled by how significant each centre is, NOT measurements. Congestion
# results on the Sydney dataset are therefore illustrative; the Hume dataset
# carries real charger counts, and stage 3 replaces these with open data.
SYDNEY_STATION_CAPACITY = {
    "Penrith":        (6, 150.0),
    "Box Hill":       (2, 50.0),
    "Blacktown":      (6, 150.0),
    "Parramatta":     (8, 150.0),
    "Olympic Park":   (6, 150.0),
    "Burwood":        (4, 50.0),
    "Randwick":       (4, 50.0),
    "Bondi Junction": (4, 50.0),
    "Manly":          (2, 50.0),
    "Chatswood":      (6, 150.0),
    "Macquarie Pk":   (4, 50.0),
    "Hurstville":     (4, 50.0),
    "Bankstown":      (4, 50.0),
    "Campbelltown":   (4, 50.0),
    "Ryde":           (3, 50.0),
    "Strathfield":    (4, 50.0),
}


def load_sydney_raw():
    raw = DATA / "sydney" / "raw"
    names, has_station, prices = [], [], []
    for line in (raw / "Locations.txt").read_text().strip().splitlines():
        name, charger, price = [p.strip() for p in line.split(",")]
        names.append(name)
        has_station.append(charger == "1")
        prices.append(float(price))

    # Read LINE PER ROW, which is the author's evident intent. Note that the
    # legacy C++ read this as a flat token stream via `infile >> value`, so the
    # one short row silently shifted every subsequent value by one position and
    # corrupted the last three suburbs' distances.
    rows = []
    for line in (raw / "Weights.txt").read_text().strip().splitlines():
        vals = [float(v) for v in line.split()]
        vals += [0.0] * (len(names) - len(vals))  # pad the malformed row
        rows.append(vals)
    return names, has_station, prices, rows


def build_sydney():
    names, has_station, prices, rows = load_sydney_raw()
    index = {n: i for i, n in enumerate(names)}
    n = len(names)

    # Collapse the directed matrix into undirected pairs, keeping whichever
    # direction carries a value.
    pairs: dict[tuple[int, int], float] = {}
    for i in range(n):
        for j in range(n):
            if rows[i][j] > 0:
                pairs[(min(i, j), max(i, j))] = rows[i][j]

    applied = []
    for a, b, dist in SYDNEY_EDGE_CORRECTIONS:
        key = (min(index[a], index[b]), max(index[a], index[b]))
        if dist is None:
            if pairs.pop(key, None) is not None:
                applied.append(f"dropped {a} <-> {b}")
        else:
            pairs[key] = dist
            applied.append(f"set {a} <-> {b} = {dist} km")

    nodes = []
    for i, name in enumerate(names):
        chargers, power = SYDNEY_STATION_CAPACITY.get(name, (0, 0.0))
        nodes.append(
            {
                "id": i,
                "name": name,
                "has_station": int(has_station[i]),
                # -1 in the legacy file meant "no charger"; 0 meant "free".
                "price_per_kwh": f"{max(prices[i], 0.0):.2f}" if has_station[i] else "0.00",
                "chargers": chargers if has_station[i] else 0,
                "power_kw": f"{power:.1f}" if has_station[i] else "0.0",
            }
        )

    edges = [
        {"from_id": a, "to_id": b, "distance_km": f"{d:.1f}"}
        for (a, b), d in sorted(pairs.items())
    ]
    return nodes, edges, applied


def sydney_demands(nodes, seed=20260817):
    """Top-up missions: 'I am at X and need N kWh -- where should I charge?'

    This is the question the legacy Sydney project actually asked (its Task 6
    and 7). It is encoded as origin == destination with a non-zero
    required_kwh, which the C++ core reads as a round-trip top-up mission.
    """
    rng = random.Random(seed)
    out = []
    for i in range(100):
        origin = rng.randrange(len(nodes))
        battery = rng.choice([45.0, 60.0, 75.0, 90.0])
        out.append(
            {
                "id": 1000 + i,
                "origin_id": origin,
                "destination_id": origin,
                "battery_kwh": f"{battery:.1f}",
                # Enough charge to move around the metro network freely.
                "soc_kwh": f"{battery * rng.uniform(0.35, 0.75):.1f}",
                "efficiency_kwh_per_100km": f"{EFFICIENCY_KWH_PER_100KM:.1f}",
                # Legacy range was 10-60 kWh; preserved.
                "required_kwh": f"{rng.uniform(10.0, 60.0):.1f}",
                "release_hour": f"{departure_hour(rng):.2f}",
            }
        )
    return out


# --------------------------------------------------------------------------
# Hume corridor
# --------------------------------------------------------------------------

# Lifted verbatim from the legacy ev_charging1/Constant.h, where they lived as
# three parallel `const` arrays indexed by an int -- a design with nothing to
# stop an index mismatch. distance_from_prev_km[i] was the leg from town i-1
# to town i, which only works because the corridor is a straight chain.
HUME_TOWNS = [
    # (name, distance_from_previous_km, chargers, price_per_kwh)
    ("Sydney",        0,  10, 0.45),
    ("Campbelltown",  57,  4, 0.48),
    ("Mittagong",     60,  3, 0.55),
    ("Goulburn",      83,  4, 0.52),
    ("Yass",          86,  2, 0.58),
    ("Gundagai",      99,  3, 0.60),
    ("Holbrook",     115,  2, 0.62),
    ("Albury",        62,  4, 0.50),
    ("Wangaratta",    74,  3, 0.56),
    ("Euroa",         87,  3, 0.59),
    ("Wallan",       106,  2, 0.54),
    ("Melbourne",     62,  8, 0.44),
]

# Highway fast chargers. Charger counts above are the coursework's own figures;
# prices are synthetic (the legacy project never modelled money) but sit in a
# realistic band for Australian highway DC charging.
HUME_POWER_KW = 150.0


def build_hume():
    nodes, edges = [], []
    for i, (name, dist, chargers, price) in enumerate(HUME_TOWNS):
        nodes.append(
            {
                "id": i,
                "name": name,
                "has_station": 1,
                "price_per_kwh": f"{price:.2f}",
                "chargers": chargers,
                "power_kw": f"{HUME_POWER_KW:.1f}",
            }
        )
        if i > 0:
            edges.append(
                {"from_id": i - 1, "to_id": i, "distance_km": f"{float(dist):.1f}"}
            )
    return nodes, edges


def hume_demands(seed=20260817):
    """Journey missions along the corridor, charging en route as needed.

    Mirrors the legacy DemandGenerator (150-200 vehicles, capacity 350-550 km,
    remaining range 300 km to capacity) but converts range to kWh, and adds
    northbound traffic so the corridor is exercised in both directions rather
    than every vehicle starting at Sydney.
    """
    rng = random.Random(seed)
    out = []
    count = rng.randint(150, 200)
    last = len(HUME_TOWNS) - 1
    for i in range(count):
        capacity_km = rng.uniform(350.0, 550.0)
        remain_km = rng.uniform(300.0, capacity_km)
        southbound = rng.random() < 0.8
        if southbound:
            origin, destination = 0, rng.randint(1, last)
        else:
            origin, destination = last, rng.randint(0, last - 1)
        out.append(
            {
                "id": 200 + i,
                "origin_id": origin,
                "destination_id": destination,
                "battery_kwh": f"{capacity_km * EFFICIENCY_KWH_PER_100KM / 100.0:.1f}",
                "soc_kwh": f"{remain_km * EFFICIENCY_KWH_PER_100KM / 100.0:.1f}",
                "efficiency_kwh_per_100km": f"{EFFICIENCY_KWH_PER_100KM:.1f}",
                "required_kwh": "0.0",
                "release_hour": f"{departure_hour(rng):.2f}",
            }
        )
    return out


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------


def write_csv(path: Path, rows: list[dict], header_comment: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as fh:
        fh.write(f"# {header_comment}\n")
        writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"  wrote {path.relative_to(ROOT)} ({len(rows)} rows)")


def check_connected(nodes, edges) -> tuple[int, int]:
    adjacency: dict[int, set[int]] = {n["id"]: set() for n in nodes}
    for e in edges:
        adjacency[int(e["from_id"])].add(int(e["to_id"]))
        adjacency[int(e["to_id"])].add(int(e["from_id"]))
    seen, stack = {nodes[0]["id"]}, [nodes[0]["id"]]
    while stack:
        for nxt in adjacency[stack.pop()]:
            if nxt not in seen:
                seen.add(nxt)
                stack.append(nxt)
    return len(seen), len(nodes)


def main() -> None:
    print("Sydney metro:")
    nodes, edges, applied = build_sydney()
    for note in applied:
        print(f"  correction: {note}")
    reached, total = check_connected(nodes, edges)
    print(f"  connectivity: {reached}/{total} nodes reachable")
    assert reached == total, "Sydney network is disconnected"
    write_csv(DATA / "sydney" / "nodes.csv", nodes,
              "Sydney metro nodes. Generated by tools/build_datasets.py -- do not edit by hand.")
    write_csv(DATA / "sydney" / "edges.csv", edges,
              "Undirected road distances in km. See PROVENANCE.md for corrections.")
    write_csv(DATA / "sydney" / "demands.csv", sydney_demands(nodes),
              "Top-up missions (origin == destination, required_kwh > 0). Seeded, reproducible.")

    print("\nHume corridor:")
    hnodes, hedges = build_hume()
    reached, total = check_connected(hnodes, hedges)
    print(f"  connectivity: {reached}/{total} nodes reachable")
    assert reached == total, "Hume corridor is disconnected"
    write_csv(DATA / "hume" / "nodes.csv", hnodes,
              "Hume corridor towns. Charger counts from the legacy coursework; prices synthetic.")
    write_csv(DATA / "hume" / "edges.csv", hedges,
              "Undirected leg distances in km along the Sydney-Melbourne corridor.")
    write_csv(DATA / "hume" / "demands.csv", hume_demands(),
              "Journey missions (origin != destination, required_kwh == 0). Seeded, reproducible.")
    print("\nDone.")


if __name__ == "__main__":
    main()
