# Sydney real dataset — provenance

## Source

**OpenStreetMap**, via the Overpass API: every `amenity=charging_station` node and
way inside the bounding box `-34.15 150.60 -33.55 151.35` (greater Sydney), fetched
once on 2026-08-17. 238 elements came back, all nodes.

Regenerate it with:

```bash
python3 tools/fetch_real_network.py --bbox -34.15 150.60 -33.55 151.35 \
    --name sydney-real --out data/sydney-real \
    --merge-within-m 50 --candidate-sites 20 --candidate-clearance-km 4
```

The raw Overpass response is cached under `.cache/` and deliberately not committed:
it is fetched data rather than source, and `.gitignore` keeps it out. Keep your copy
— while it exists the script makes no network call at all, which is the point of the
cache. Both services are volunteer-run.

Contrast with `data/sydney`, which is the legacy coursework matrix: 24 suburbs with
**estimated** charger counts. This one carries what OSM actually records.

## What the raw data needed

Real data is not clean data. Four things had to be handled, each reported by the
script as it runs.

### 11 co-located duplicates

OSM records the same physical site more than once — as a node and again as the car
park polygon around it, or as two adjacent banks of chargers. Eleven pairs here sit
within 50 m of each other, which produces **edges of length zero**, and the loader
correctly refuses those:

```
error: network: edge on line 295 has a non-positive distance
```

They are merged into one site with the combined charger count, which is what a driver
finds on arrival. Flooring the distance to some small number would have silenced the
error while leaving two sites in the model where there is one. 238 elements → **227
distinct sites**.

### A disconnected pocket

Nearest-4 left the graph in more than one piece. Rather than asking you to guess a
larger `--neighbours` and densify the whole network to fix one corner of it, the
components are joined by **2 bridging edges**, each the shortest available crossing.
The result is 238/238 reachable.

### One capacity that was really a power rating

`node/8169947525` ("Evie Networks") tagged `capacity=350`. The median across this
extract is **2** and the next-largest site has **14**, so 350 bays at one suburban node
is not a plausible reading — and 350 kW is a standard charger rating, while the same
node's own power tag says 22 kW. Someone put the kilowatts in the capacity field.

Left in, that one node would have held **350 of the network's 952 chargers — 37% of
total capacity at a single point.** Every congestion result and every siting
recommendation drawn from this dataset would have been shaped by it, and nothing would
have looked obviously wrong.

The tag is reported loudly and then discarded, falling back to socket counts exactly as
a station with no capacity tag does. Discarding rather than clamping is the point:
clamping to 60 would invent a number, whereas falling back says only that the tag told
us nothing usable. Corrected: **606 chargers, max 14, median 2.**

There is no clean physical bound here of the sort great-circle distance gives for road
lengths, so `MAX_PLAUSIBLE_CHARGERS` is a judgement (60) rather than a law. It sits well
above the largest genuine charging site anywhere, so it only catches this failure mode.

### No candidate sites at all

A network built from where chargers already *are* contains no station-less nodes, and
`evnet site` ranks exactly those — so on real data the toolkit's headline question had
nothing to answer. **11 candidate sites** are laid on a grid across the network's own
extent, at least 4 km clear of any existing charger. A grid is crude but unbiased;
hand-picking a shortlist would presume the answer.

They only ever *add*. The station-to-station subgraph is identical whether candidates
are present or not — 573 edges either way, verified at 5, 11, 20 and 40 candidates on
this exact extract. That invariant is not decorative: built naively, injecting
candidates deleted 10 real edges through displaced nearest-k shortlists and 3 more
through bridging, and siting then reported that every possible new station made the
network worse. It was measuring the damage its own candidate set had done.

## Honest limits

**Road distances here are estimated, not surveyed.** This dataset was built with
`--no-osrm`, so each edge is the great-circle distance inflated by 1.35 — a typical
detour factor. The header comment in `edges.csv` says so.

To replace them with real routed distances, drop `--no-osrm` from the command above.
OSRM's public instance caps a request by coordinate count, so the matrix is assembled
from 45×45 tiles with a second's pause between them: 36 requests, cached afterwards.
The edges change; nothing else does.

**Prices are an assumption.** OSM does not record tariffs, so every station carries the
`--price` default of $0.55/kWh. Charger counts and power ratings *are* from OSM tags,
parsed from the several inconsistent forms it uses — `charging_station:output`, bare
watts, semicolon-separated lists, per-socket subtags. Where a station carries no power
tag at all the parser falls back pessimistically rather than guessing high.

**Station names are OSM's**, which means many are an operator rather than a place —
"Tesla Supercharger", "Chargefox", "NRMA" — and repeat. The schema requires unique
names, so duplicates are numbered (`Chargefox #3`).

## Demands

`demands.csv` holds 400 point-to-point journeys between real stations, never to or
from a candidate site — a journey starting at a car park that does not exist yet is
not a journey. Generated with a fixed seed (`20260817`), so runs are reproducible.

Battery capacity is derived from the network's own extent rather than fixed. Sized for
a corridor, every vehicle in a metro network finishes without charging and the whole
policy comparison is vacuous; here a full charge covers roughly 40–75% of the longest
journey, so long trips need a stop and short ones do not.

## What it shows

Running all five planners over this network (`evnet compare`) turns up something the
shipped datasets do not: **the greedy planners strand vehicles that the optimal planner
gets home.** `cheapest` loses 9 of 400 and `generalised` 8, while `optimal`, `farthest`
and `min-wait` complete every trip. The greedy failures are the thrashing pattern —
many tiny top-ups, each locally cheapest, until the vehicle runs out of moves.
