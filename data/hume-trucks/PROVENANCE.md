# Hume heavy-vehicle dataset — provenance

The toolkit's second domain: solo heavy-vehicle drivers on the Hume, Sydney to
Melbourne, under the Heavy Vehicle National Law's standard hours, resting at the
corridor's heavy-vehicle rest areas. `domain.txt` says `truck`.

**One input here is estimated rather than recorded: how many trucks each rest area
holds.** Everything reported from this dataset is therefore swept across a scale on
those numbers, not quoted at face value. See [Capacity](#capacity-the-one-estimated-input).

## Sources

**Rest areas.** National Freight Data Hub, *National Formal Rest Areas*
([catalogue](https://catalogue.data.infrastructure.gov.au/dataset/national-formal-rest-areas)),
the monthly national compilation of state and territory rest-area data. Downloaded
by hand as `nra.csv` on 2026-09-21: 5,036 rows nationally, 112 on the Hume.

*Licence.* The publisher states none; the catalogue lists it as unspecified. So the
national file is **not** committed. What is committed is `rest_areas.csv`: the 112
Hume rows only, with this study's decision on each. The NSW source data this
compilation draws on is published by Transport for NSW under CC-BY.

**Corridor.** The twelve towns and road distances of `data/hume`, 891 km in all.

**Fatigue rules.** NHVR, *Work and rest requirements*, standard hours for a solo
driver: at most 12 hours' work in any 24, with at least 7 continuous hours of
stationary rest.

## Rebuilding

```bash
# needs the downloaded national file
python3 tools/build_truck_network.py extract --nra path/to/nra.csv
# needs only rest_areas.csv -- CI runs this and fails on any difference
python3 tools/build_truck_network.py build
```

## Which rows are used

Of the 112 Hume rows, 40 are used. Every row stays in `rest_areas.csv` with the
reason it was left out:

| rows | decision |
|---:|---|
| 40 | **used** |
| 45 | serves Melbourne → Sydney (Eastbound / Northbound) |
| 11 | type is silent about trucks and the heavy-vehicle flag is N — Victoria's service centres |
| 9 | type says light vehicles only, although flagged for heavy vehicles |
| 4 | no direction of travel recorded |
| 1 | cars only |
| 1 | a weighbridge |
| 1 | on the bypassed Old Hume Highway |

The rule, applied consistently: **a site's type decides wherever it says what
vehicles it takes; the `heavy_vehicle_area` flag decides only where the type is
silent.** The flag cannot be trusted alone — nine "Light Vehicle Only Rest Area"
rows are flagged `Y`.

That rule has a cost worth stating. Victoria's eleven Hume service centres are
flagged `N` while NSW's are flagged `Y`, and "Service Centre" says nothing about
trucks either way. They are excluded, so **the Victorian half may be under-supplied
relative to reality.** The capacity sweep bounds how much that can matter.

### Direction

Trucks travel Sydney → Melbourne only. The engine's stations have no direction, and
a southbound bay is no use to a northbound truck. NSW labels the Hume by bearing
("Westbound" is away from Sydney) and Victoria by compass, and paired sites agree:
a site named "…Truck Parking Bay Southbound" is recorded as Westbound. Modelling
both directions needs direction-aware stations; it is future work, not a detail.

## Capacity: the one estimated input

The dataset records no capacity of any kind. Bays are assumed by site type:

| site type | sites | bays each | basis |
|---|---:|---:|---|
| Class 1 or 2 heavy vehicle rest area (NSW) | 2 | 20 | Austroads minimum |
| Class 3 or 4 (NSW) | 3 | 10 | Austroads minimum |
| Class 5 (NSW) | 9 | 5 | Austroads minimum |
| Truck parking bay | 9 | 5 | estimate, sized as Class 5 |
| Other rest area, heavy vehicles flagged | 1 | 5 | estimate, smallest designed class |
| Service centre (NSW) | 2 | 20 | estimate, sized as Class 1–2 |
| Cars and trucks (VIC) | 11 | 10 | estimate, sized as Class 3–4 |
| Informal heavy rest area | 3 | 2 | estimate, no design minimum exists |

The Austroads minimums are from AP-R591-19, *Guidelines for the Provision of Heavy
Vehicle Rest Area Facilities*, Table 4.1: Class 1–2 "20+", Class 3–4 "10–15",
Class 5 "5+". Those are **minimums** for designed sites, and they cover only **115
of the 321 assumed bays**; the other 206 are estimates.

## Placement on the corridor

The corridor is drawn as straight chords between towns, and each rest area is
projected onto the nearest chord and placed along that leg's *road* distance in
proportion. The real Hume curves away from those chords: the largest offset is 16.4
km, at Reddy Creek. A first cut that treated anything beyond 10 km as mis-coded
dropped eight genuine Hume sites — Coolac, Mungi Mungi Hill and Conroys Gap among
them — and with them about 15% of the bays. The tolerance is 25 km.

Positions along a leg are therefore approximate to a few kilometres. At 80 km/h that
is a few minutes of a driver's clock.

## Demand: a scenario, not an observation

300 trucks, Sydney → Melbourne, released uniformly over 24 hours, each having
already worked between 0 and 9 of its 12 hours. Seeded, so reproducible. The fleet
size is a scenario parameter, not a traffic count, and is swept like capacity.

Starting part-way through a shift is what makes the corridor interesting: 891 km at
80 km/h is 11.1 hours, inside a single 12-hour allowance, so a fleet of fresh
drivers would barely need to rest at all.

## What the model leaves out

- **Short breaks.** Standard hours also require 15 minutes' rest by 5.5 hours of
  work, 30 by 8 and 60 by 11. They are further clocks the engine's single resource
  cannot carry, and they occupy bays too — so any shortage found here is a lower
  bound.
- **Non-driving work.** Loading, fuelling and paperwork count as work; the starting
  level stands in for work done before the corridor.
- **Traffic in the other direction, and trucks joining mid-corridor.**
