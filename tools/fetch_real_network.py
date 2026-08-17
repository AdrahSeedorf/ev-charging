#!/usr/bin/env python3
"""Build a network from real charging stations and real road distances.

Replaces the estimated figures the shipped Sydney dataset carries with surveyed
ones. Two sources, both free and neither needing a key:

  * OpenStreetMap via the Overpass API -- `amenity=charging_station` nodes and ways
    inside a bounding box, with their position, operator, socket count and power.
  * OSRM via its public demo server -- the road distance matrix between them, so
    edges are driving distances rather than straight lines.

Output is the same nodes.csv / edges.csv / demands.csv schema the rest of the
toolkit reads, so nothing downstream changes.

    # Greater Sydney. Note the space-separated bounding box: south west north east.
    python3 tools/fetch_real_network.py --bbox -34.15 150.60 -33.55 151.35 \\
        --name sydney-real --out data/sydney-real

    # Rehearse without touching the network, using the bundled fixture
    python3 tools/fetch_real_network.py --offline-fixture tests/fixtures/overpass_sample.json \\
        --name fixture --out /tmp/fixture

NOTE ON ETIQUETTE. Both services are volunteer-run and rate-limited. Overpass is
called once per run. OSRM limits a request by coordinate count, so its distance matrix
is assembled from tiles of 45x45 with a second's pause between them -- 36 requests for
a 238-station network. Every response is cached, and nothing is re-requested while a
cache file exists. Do not remove that behaviour to "get fresher data": an unthrottled
loop against either service is how a client gets banned. A study substantially larger
than a city wants a self-hosted OSRM.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
import shutil
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

OVERPASS_URL = "https://overpass-api.de/api/interpreter"
# Tried in order. FOSSGIS's instance is the better-maintained of the two; the
# project-osrm demo host is kept as a fallback because it is the one most
# documentation points at, but it has been known to fail its TLS handshake outright.
OSRM_ENDPOINTS = [
    ("FOSSGIS", "https://routing.openstreetmap.de/routed-car/table/v1/driving/"),
    ("project-osrm demo", "https://router.project-osrm.org/table/v1/driving/"),
]

# Coordinates permitted in a single table request. The public instances cap this near
# 100, so a request is built from two blocks of this size at most.
OSRM_BLOCK = 45
OSRM_MIN_INTERVAL = 1.1  # seconds; both hosts ask for no more than 1 request/second
USER_AGENT = "ev-network-toolkit/0.1 (+https://github.com/; research use, one request per run)"

EFFICIENCY_KWH_PER_100KM = 18.0
DAY_LENGTH_HOURS = 16.0



# --------------------------------------------------------------------------
# Transport
# --------------------------------------------------------------------------


def _looks_like_tls_failure(error: BaseException) -> bool:
    text = str(getattr(error, "reason", error)).lower()
    return any(token in text for token in
               ("ssl", "tls", "handshake", "certificate", "cert_", "eof occurred"))


def _via_urllib(url: str, data: bytes | None) -> dict:
    request = urllib.request.Request(url, data=data, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=180) as response:
        return json.loads(response.read().decode())


def _via_curl(url: str, data: bytes | None) -> dict:
    """Same request through curl.

    Python on macOS is frequently linked against an OpenSSL that some hosts refuse to
    negotiate with, which surfaces as SSLV3_ALERT_HANDSHAKE_FAILURE against a server
    that is demonstrably up. curl links a different TLS stack, so it commonly succeeds
    where urllib cannot -- and it is already present on macOS and most Linux images.
    """
    curl = shutil.which("curl")
    if curl is None:
        raise RuntimeError("curl is not installed, so there is no alternative transport")

    command = [curl, "-sS", "--fail-with-body", "--max-time", "180",
               "-A", USER_AGENT, "-H", "Accept: application/json"]
    handle = None
    try:
        if data is not None:
            handle = tempfile.NamedTemporaryFile("wb", suffix=".post", delete=False)
            handle.write(data)
            handle.close()
            command += ["--data-binary", f"@{handle.name}"]
        command.append(url)
        finished = subprocess.run(command, capture_output=True, timeout=200)
    finally:
        if handle is not None:
            Path(handle.name).unlink(missing_ok=True)

    if finished.returncode != 0:
        raise RuntimeError(f"curl exited {finished.returncode}: "
                           f"{finished.stderr.decode(errors='replace').strip()[:300]}")
    try:
        return json.loads(finished.stdout.decode())
    except json.JSONDecodeError as error:
        raise RuntimeError(f"curl returned something that is not JSON: "
                           f"{finished.stdout.decode(errors='replace')[:200]}") from error


TRANSPORT = "auto"  # set from --transport
_TLS_BROKEN = False  # latched once Python's TLS has failed, so it is not retried 36 times


def request_json(url: str, data: bytes | None, label: str) -> dict:
    """One HTTP call, falling back to curl when Python's TLS will not negotiate.

    The fallback LATCHES. Python's TLS stack either can negotiate with a host or it
    cannot; it does not change its mind between requests. Retrying it per call meant a
    36-tile run printed the same handshake failure 36 times and paid the timeout each
    time, which buried the real progress output.
    """
    global _TLS_BROKEN
    if TRANSPORT == "curl" or (TRANSPORT == "auto" and _TLS_BROKEN):
        return _via_curl(url, data)
    try:
        return _via_urllib(url, data)
    except (ssl.SSLError, urllib.error.URLError, OSError) as error:
        if TRANSPORT != "auto" or not _looks_like_tls_failure(error):
            raise
        _TLS_BROKEN = True
        print(f"  {label}: python TLS cannot negotiate ({getattr(error, 'reason', error)})")
        print(f"  {label}: switching to curl for the rest of this run")
        return _via_curl(url, data)


# --------------------------------------------------------------------------
# Overpass: where the chargers are
# --------------------------------------------------------------------------


def overpass_query(bbox: str) -> str:
    """`bbox` is south,west,north,east -- Overpass's own ordering.

    Both nodes and ways are collected: a charging site mapped as a car park polygon
    is as real as one mapped as a point, and `out center` gives a usable position for
    either.
    """
    return f"""
[out:json][timeout:90];
(
  node["amenity"="charging_station"]({bbox});
  way["amenity"="charging_station"]({bbox});
);
out center tags;
""".strip()


def fetch(url: str, data: bytes | None, cache: Path, label: str) -> dict:
    """One request, cached. A cache hit makes no network call at all."""
    if cache.exists():
        print(f"  {label}: using cached {cache}")
        return json.loads(cache.read_text())

    print(f"  {label}: requesting (this is the only call; the result is cached)")
    try:
        payload = request_json(url, data, label)
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")[:400]
        raise SystemExit(
            f"error: {label} returned HTTP {error.code}.\n"
            f"  {detail}\n"
            f"  Both services are volunteer-run and rate-limited. Wait a few minutes "
            f"rather than retrying immediately."
        ) from error
    except (urllib.error.URLError, RuntimeError, OSError) as error:
        raise SystemExit(f"error: could not reach {label}: "
                         f"{getattr(error, 'reason', error)}") from error

    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps(payload))
    print(f"  {label}: cached to {cache}")
    return payload


def parse_power_kw(tags: dict) -> float:
    """Best available power rating, in kW.

    OSM records this inconsistently. In rough order of reliability:
    `charging_station:output`, then any `socket:<type>:output`, then a bare
    `socket:<type>` count with no rating. Values arrive as "50 kW", "50000 W",
    "22.1", or occasionally as a ";"-separated list of options.
    """
    def to_kw(raw: str) -> float | None:
        best = None
        for part in str(raw).replace(",", ".").split(";"):
            token = part.strip().lower().rstrip(".")
            if not token:
                continue
            watts = token.endswith("w") and not token.endswith("kw")
            number = "".join(ch for ch in token if ch.isdigit() or ch == ".")
            try:
                value = float(number)
            except ValueError:
                continue
            if watts:
                value /= 1000.0
            if value > 0 and (best is None or value > best):
                best = value
        return best

    for key in ("charging_station:output", "output", "maxpower", "power"):
        if key in tags:
            value = to_kw(tags[key])
            if value:
                return value
    for key, raw in tags.items():
        if key.startswith("socket:") and key.endswith(":output"):
            value = to_kw(raw)
            if value:
                return value
    # Unrated: assume a slow AC post rather than a fast DC one, so the model does not
    # flatter the network.
    return 22.0


def parse_capacity(tags: dict) -> int:
    """Number of vehicles that can charge at once."""
    for key in ("capacity", "charging_station:capacity"):
        if key in tags:
            try:
                value = int(float(str(tags[key]).split(";")[0].strip()))
                if value > 0:
                    return value
            except ValueError:
                pass
    # Sum the per-socket counts if they are present.
    total = 0
    for key, raw in tags.items():
        if key.startswith("socket:") and not key.endswith(("output", "voltage", "amperage")):
            try:
                total += int(float(str(raw).strip()))
            except ValueError:
                continue
    return max(total, 1)


def station_name(tags: dict, index: int) -> str:
    for key in ("name", "operator", "brand", "network"):
        if tags.get(key):
            return str(tags[key])[:44]
    return f"Station {index}"


def extract_stations(payload: dict, max_stations: int, seed: int) -> list[dict]:
    stations = []
    for element in payload.get("elements", []):
        centre = element.get("center") or element
        lat, lon = centre.get("lat"), centre.get("lon")
        if lat is None or lon is None:
            continue
        tags = element.get("tags", {})
        stations.append({
            "osm_id": f"{element.get('type', 'node')}/{element.get('id', '?')}",
            "raw_name": station_name(tags, len(stations) + 1),
            "lat": float(lat),
            "lon": float(lon),
            "chargers": parse_capacity(tags),
            "power_kw": parse_power_kw(tags),
            "operator": tags.get("operator", ""),
        })

    if not stations:
        raise SystemExit("error: Overpass returned no charging stations for that bounding box")

    # Names repeat constantly in OSM ("Tesla Supercharger" many times over), and the
    # schema requires them unique, so disambiguate by suffix.
    seen: dict[str, int] = {}
    for station in stations:
        base = station["raw_name"]
        seen[base] = seen.get(base, 0) + 1
        station["name"] = base if seen[base] == 1 else f"{base} #{seen[base]}"

    if len(stations) > max_stations:
        # Deterministic thinning, and SAID OUT LOUD -- a silent cap would make the
        # study look complete when it is a sample.
        print(f"  note: {len(stations)} stations found, sampling {max_stations}. "
              f"Raise --max-stations to keep more (routing is tiled, so the cost is "
              f"roughly one second per 45x45 block).")
        random.Random(seed).shuffle(stations)
        stations = stations[:max_stations]
        stations.sort(key=lambda s: (s["lat"], s["lon"]))

    for index, station in enumerate(stations):
        station["id"] = index
    return stations


# --------------------------------------------------------------------------
# OSRM: how far apart they are by road
# --------------------------------------------------------------------------


def osrm_tile(base: str, stations: list[dict], rows: list[int],
              cols: list[int]) -> list[list[float | None]]:
    """One sub-matrix: road distances from every station in `rows` to every one in `cols`.

    Only the coordinates involved are sent, because the public instances limit a
    request by coordinate count rather than by matrix size.
    """
    indices = list(dict.fromkeys(rows + cols))  # dedup, order preserved
    position = {node: i for i, node in enumerate(indices)}
    coords = ";".join(f"{stations[i]['lon']:.6f},{stations[i]['lat']:.6f}" for i in indices)
    query = urllib.parse.urlencode({
        "annotations": "distance",
        "sources": ";".join(str(position[r]) for r in rows),
        "destinations": ";".join(str(position[c]) for c in cols),
    })

    payload = request_json(f"{base}{coords}?{query}", None, "OSRM")
    if payload.get("code") != "Ok":
        raise RuntimeError(f"OSRM replied {payload.get('code')}: {payload.get('message')}")
    distances = payload.get("distances")
    if not distances:
        raise RuntimeError("OSRM returned no distance matrix")
    return [[(None if v is None else float(v) / 1000.0) for v in row] for row in distances]


def fetch_road_distances(stations: list[dict], cache: Path) -> list[list[float | None]]:
    """Full road distance matrix, assembled from tiled requests.

    Tiling rather than sampling. An earlier version capped the station count to fit a
    single request, which on Greater Sydney meant discarding 148 of 238 real stations
    -- most of the data, thrown away to avoid a second HTTP call. Blocks of 45 keep
    every request inside the coordinate limit, and a 238-station network needs 36 of
    them, which at one per second is under a minute.
    """
    if cache.exists():
        print(f"  OSRM: using cached {cache}")
        return json.loads(cache.read_text())

    count = len(stations)
    blocks = [list(range(i, min(i + OSRM_BLOCK, count))) for i in range(0, count, OSRM_BLOCK)]
    tiles = [(r, c) for r in blocks for c in blocks]

    # Settle on an endpoint using the first tile, so a dead host costs one request.
    base = None
    first = None
    problems = []
    for label, candidate in OSRM_ENDPOINTS:
        try:
            print(f"  OSRM: trying {label}")
            first = osrm_tile(candidate, stations, tiles[0][0], tiles[0][1])
            base = candidate
            print(f"  OSRM: using {label}, {len(tiles)} tile request(s) to make")
            break
        except (urllib.error.URLError, urllib.error.HTTPError, RuntimeError,
                ssl.SSLError, OSError) as error:
            reason = getattr(error, "reason", error)
            print(f"  OSRM: {label} unavailable ({reason})")
            problems.append(f"{label}: {reason}")

    if base is None:
        raise SystemExit(
            "error: no OSRM endpoint could be reached.\n  "
            + "\n  ".join(problems)
            + "\n\n  If those mention TLS or a handshake, try --transport curl."
            "\n  Otherwise re-run with --no-osrm to estimate distances from geometry."
            "\n  Straight-line distance times 1.35 is a reasonable stand-in, and the"
            "\n  output records that the edges are estimated rather than measured."
        )

    matrix: list[list[float | None]] = [[None] * count for _ in range(count)]

    def store(rows, cols, tile):
        for i, r in enumerate(rows):
            for j, c in enumerate(cols):
                matrix[r][c] = tile[i][j]

    store(tiles[0][0], tiles[0][1], first)
    for number, (rows, cols) in enumerate(tiles[1:], start=2):
        time.sleep(OSRM_MIN_INTERVAL)
        print(f"  OSRM: tile {number}/{len(tiles)}", end="\r", flush=True)
        try:
            store(rows, cols, osrm_tile(base, stations, rows, cols))
        except (urllib.error.URLError, urllib.error.HTTPError, RuntimeError,
                ssl.SSLError, OSError) as error:
            # A tile that fails leaves its pairs as None, and build_edges falls back to
            # geometry for those. Better a partly-measured matrix than none at all.
            print(f"\n  OSRM: tile {number} failed ({getattr(error, 'reason', error)}); "
                  f"those pairs fall back to geometry")
    print(f"  OSRM: {len(tiles)} tile(s) done" + " " * 20)

    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps(matrix))
    print(f"  OSRM: cached to {cache}")
    return matrix


def great_circle_km(a: dict, b: dict) -> float:
    radius = 6371.0088
    lat1, lat2 = math.radians(a["lat"]), math.radians(b["lat"])
    dlat = lat2 - lat1
    dlon = math.radians(b["lon"] - a["lon"])
    h = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    return 2 * radius * math.asin(math.sqrt(min(1.0, h)))


def build_edges(stations: list[dict], matrix: list[list[float]] | None,
                neighbours: int) -> tuple[list[dict], list[str]]:
    """Keep each station's `neighbours` nearest others, so the graph stays sparse.

    A complete graph would make every station adjacent to every other, which is both
    unlike a road network and quadratic in edges. Nearest-k keeps it sparse and
    connected, and the toolkit's own validator will complain if it is not.
    """
    notes: list[str] = []
    count = len(stations)
    pairs: dict[tuple[int, int], float] = {}

    for i in range(count):
        distances = []
        for j in range(count):
            if i == j:
                continue
            road = matrix[i][j] if matrix else None
            straight = great_circle_km(stations[i], stations[j])
            if road is None or road <= 0.0:
                # Fall back to a straight line inflated by a typical detour factor.
                #
                # Only worth a note when a matrix EXISTS and this pair is missing from
                # it -- an island, or a gap in the mapping. When routing was skipped
                # altogether the header already says every distance is estimated, and
                # a note per pair buries the real warnings under hundreds of lines.
                road = straight * 1.35
                if matrix:
                    notes.append(f"no route {stations[i]['name']} -> "
                                 f"{stations[j]['name']}, estimated from geometry")
            elif road < straight:
                # Geometrically impossible; trust the geometry, since it is the harder
                # constraint. Usually means OSRM snapped a coordinate oddly.
                notes.append(f"{stations[i]['name']} -> {stations[j]['name']}: road "
                             f"{road:.1f} km < straight line {straight:.1f} km, clamped")
                road = straight
            distances.append((road, j))
        distances.sort()
        for road, j in distances[:neighbours]:
            key = (min(i, j), max(i, j))
            pairs[key] = min(pairs.get(key, float("inf")), road)

    edges = [{"from_id": a, "to_id": b, "distance_km": f"{d:.2f}"}
             for (a, b), d in sorted(pairs.items())]
    return edges, notes


def connected(stations, edges) -> int:
    adjacency: dict[int, set[int]] = {s["id"]: set() for s in stations}
    for edge in edges:
        adjacency[int(edge["from_id"])].add(int(edge["to_id"]))
        adjacency[int(edge["to_id"])].add(int(edge["from_id"]))
    seen, stack = {stations[0]["id"]}, [stations[0]["id"]]
    while stack:
        for nxt in adjacency[stack.pop()]:
            if nxt not in seen:
                seen.add(nxt)
                stack.append(nxt)
    return len(seen)


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------


def departure_hour(rng: random.Random) -> float:
    roll = rng.random()
    if roll < 0.45:
        hour = rng.gauss(7.5, 1.5)
    elif roll < 0.75:
        hour = rng.gauss(14.0, 2.0)
    else:
        hour = rng.uniform(0.0, DAY_LENGTH_HOURS)
    return min(max(hour, 0.0), DAY_LENGTH_HOURS)


def build_demands(stations: list[dict], count: int, seed: int) -> list[dict]:
    """Journeys sized to the network, not to a fixed idea of a car.

    Battery capacity has to be relative to how big the network actually is. Fixing it
    at a corridor-scale 350-550 km produced a metro dataset in which no vehicle ever
    needed to charge -- every planner made zero stops and the whole comparison was
    vacuous. So the range is derived from the network's own extent: a full charge
    covers roughly 40-75% of the longest journey in it, which guarantees that
    long trips need a stop and short ones do not.
    """
    rng = random.Random(seed)
    last = len(stations) - 1

    # Longest straight-line separation, inflated by a typical detour factor, as a
    # stand-in for the network's diameter by road.
    extent = 0.0
    for i, a in enumerate(stations):
        for b in stations[i + 1:]:
            extent = max(extent, great_circle_km(a, b))
    extent = max(extent * 1.35, 20.0)
    low, high = extent * 0.40, extent * 0.75
    print(f"  demands: network extent about {extent:.0f} km by road, so batteries are "
          f"sized for {low:.0f}-{high:.0f} km of range")

    out = []
    for i in range(count):
        origin = rng.randint(0, last)
        destination = rng.randint(0, last)
        while destination == origin and last > 0:
            destination = rng.randint(0, last)
        capacity_km = rng.uniform(low, high)
        # Start between a fifth and nearly full, so some trips need a stop immediately
        # and others could finish without one.
        remain_km = rng.uniform(capacity_km * 0.20, capacity_km * 0.95)
        out.append({
            "id": 5000 + i,
            "origin_id": origin,
            "destination_id": destination,
            "battery_kwh": f"{capacity_km * EFFICIENCY_KWH_PER_100KM / 100.0:.1f}",
            "soc_kwh": f"{remain_km * EFFICIENCY_KWH_PER_100KM / 100.0:.1f}",
            "efficiency_kwh_per_100km": f"{EFFICIENCY_KWH_PER_100KM:.1f}",
            "required_kwh": "0.0",
            "release_hour": f"{departure_hour(rng):.2f}",
        })
    return out


def write_csv(path: Path, rows: list[dict], comment: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        handle.write(f"# {comment}\n")
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"  wrote {path} ({len(rows)} rows)")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    # nargs="+" rather than a single value, because a bounding box in the southern or
    # western hemisphere begins with a minus sign, and argparse reads a lone
    # "-34.15,150.6,..." as an option flag rather than a value. Space-separated numbers
    # each match argparse's negative-number rule, so they survive; the tokens are then
    # rejoined and split on commas, which makes both spellings work:
    #     --bbox -34.15 150.60 -33.55 151.35
    #     --bbox=-34.15,150.60,-33.55,151.35
    parser.add_argument("--bbox", nargs="+", metavar="COORD",
                        help="south west north east, in decimal degrees "
                             "(space- or comma-separated)")
    parser.add_argument("--offline-fixture", type=Path, default=None,
                        help="use a saved Overpass response instead of calling out")
    parser.add_argument("--name", required=True, help="dataset name, used in filenames")
    parser.add_argument("--out", type=Path, required=True, help="output directory")
    parser.add_argument("--cache", type=Path, default=Path(".cache"),
                        help="where fetched responses are kept [.cache]")
    parser.add_argument("--max-stations", type=int, default=250,
                        help="cap on stations; requests are tiled, so this is about "
                             "runtime rather than any API limit [250]")
    parser.add_argument("--neighbours", type=int, default=4,
                        help="edges kept per station, nearest first [4]")
    parser.add_argument("--demands", type=int, default=400)
    parser.add_argument("--price", type=float, default=0.55,
                        help="price per kWh, since OSM rarely records tariffs [0.55]")
    parser.add_argument("--seed", type=int, default=20260817)
    parser.add_argument("--transport", choices=("auto", "urllib", "curl"), default="auto",
                        help="how to make HTTP calls. 'auto' uses Python and falls back "
                             "to curl on a TLS failure, which is the usual cure for "
                             "SSLV3_ALERT_HANDSHAKE_FAILURE against a host that is up "
                             "[auto]")
    parser.add_argument("--no-osrm", action="store_true",
                        help="skip routing; estimate distances from geometry instead")
    args = parser.parse_args()

    if not args.bbox and not args.offline_fixture:
        parser.error("one of --bbox or --offline-fixture is required")

    bbox = None
    if args.bbox:
        parts = [t for token in args.bbox for t in str(token).split(",") if t.strip()]
        if len(parts) != 4:
            parser.error(f"--bbox needs 4 numbers (south west north east), got {len(parts)}: "
                         f"{' '.join(parts)}")
        try:
            south, west, north, east = (float(v) for v in parts)
        except ValueError:
            parser.error(f"--bbox values must be numbers, got: {' '.join(parts)}")
        if not (-90 <= south < north <= 90):
            parser.error(f"--bbox latitudes must satisfy -90 <= south < north <= 90, "
                         f"got south={south} north={north}")
        if not (-180 <= west < east <= 180):
            parser.error(f"--bbox longitudes must satisfy -180 <= west < east <= 180, "
                         f"got west={west} east={east}")
        # Overpass wants south,west,north,east in exactly this order.
        bbox = f"{south},{west},{north},{east}"

    global TRANSPORT
    TRANSPORT = args.transport

    print(f"Building '{args.name}'")
    if TRANSPORT != "auto":
        print(f"  transport: {TRANSPORT} (forced)")
    if args.offline_fixture:
        print(f"  Overpass: reading fixture {args.offline_fixture}")
        payload = json.loads(args.offline_fixture.read_text())
    else:
        print(f"  Overpass: bbox south,west,north,east = {bbox}")
        payload = fetch(OVERPASS_URL,
                        urllib.parse.urlencode({"data": overpass_query(bbox)}).encode(),
                        args.cache / f"{args.name}-overpass.json", "Overpass")

    stations = extract_stations(payload, args.max_stations, args.seed)
    print(f"  {len(stations)} charging stations")

    matrix = None
    if not args.no_osrm and not args.offline_fixture:
        time.sleep(1.0)  # be a polite client between the two services
        matrix = fetch_road_distances(stations, args.cache / f"{args.name}-osrm.json")
        measured = sum(1 for row in matrix for value in row if value is not None)
        print(f"  road distances: {measured} of {len(stations) ** 2} pairs measured")
    else:
        print("  road distances: estimated from geometry (no routing requested)")

    edges, notes = build_edges(stations, matrix, args.neighbours)
    reached = connected(stations, edges)
    print(f"  {len(edges)} edges, {reached}/{len(stations)} reachable from the first station")
    if reached != len(stations):
        print(f"  note: the graph is not fully connected -- raise --neighbours above "
              f"{args.neighbours}, or narrow the bounding box")
    if notes:
        print(f"  {len(notes)} distance note(s); first few:")
        for note in notes[:5]:
            print(f"    - {note}")

    nodes = [{
        "id": s["id"], "name": s["name"], "has_station": 1,
        "price_per_kwh": f"{args.price:.2f}",
        "chargers": s["chargers"], "power_kw": f"{s['power_kw']:.1f}",
        "latitude": f"{s['lat']:.5f}", "longitude": f"{s['lon']:.5f}",
        "osm_id": s["osm_id"], "operator": s["operator"],
    } for s in stations]

    source = "an Overpass fixture" if args.offline_fixture else f"OpenStreetMap, bbox {bbox}"
    routing = "OSRM driving distances" if matrix else "great-circle distance x 1.35"
    write_csv(args.out / "nodes.csv", nodes,
              f"Charging stations from {source}. Chargers and power are OSM tags; "
              f"price is the --price assumption. Generated by tools/fetch_real_network.py.")
    write_csv(args.out / "edges.csv", edges,
              f"Road distances in km from {routing}, nearest-{args.neighbours} per station.")
    write_csv(args.out / "demands.csv", build_demands(stations, args.demands, args.seed),
              "Synthetic journeys over the real network. Seeded, reproducible.")

    print(f"\nDone. Try:\n"
          f"  ./build/debug/evnet inspect --network {args.out}\n"
          f"  ./build/debug/evnet compare --network {args.out} "
          f"--demands {args.out}/demands.csv\n"
          f"  python3 tools/render_map.py --network {args.out} --out docs --name {args.name}\n")
    print("Reminder: `inspect` runs the geometric validator over these edges. Real data "
          "is not automatically clean data.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
