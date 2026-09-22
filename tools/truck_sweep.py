#!/usr/bin/env python3
"""How do turn-aways and stranded drivers respond to rest-area capacity?

The Hume truck dataset has exactly one estimated input -- how many trucks each
rest area holds -- and a fleet that is a scenario rather than a count. So no
single run of it means much. This sweeps both: every rest area's bays scaled by
a common factor, crossed with the size of the fleet, and every planner at every
point.

The question it answers is not "how many bays is the Hume short?" -- the data
cannot say -- but "how sensitive is the outcome to capacity, and does the
answer depend on how drivers plan?"

Run:  python3 tools/truck_sweep.py [--out data/truck-sweep.csv]
"""

from __future__ import annotations

import argparse
import csv
import io
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data" / "hume-trucks"

sys.path.insert(0, str(ROOT / "tools"))
import build_truck_network as btn  # noqa: E402  (the one place the demand scenario is defined)
from saturation_sweep import find_evnet  # noqa: E402

SCALES = [0.25, 0.5, 0.75, 1.0, 1.5, 2.0]
FLEETS = [150, 300, 600, 1200]


def read(path: Path) -> list[dict]:
    with path.open(newline="") as f:
        return list(csv.DictReader(line for line in f if not line.startswith("#")))


def write_scaled_network(dest: Path, scale: float) -> int:
    nodes = read(DATA / "nodes.csv")
    total = 0
    with (dest / "nodes.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(nodes[0].keys()), lineterminator="\n")
        w.writeheader()
        for n in nodes:
            if n["has_station"] == "1":
                # Never scale a rest area out of existence: a site with one bay is
                # still a site. Rounding to nearest, so 0.5 x 5 bays is 2 or 3.
                n["servers"] = str(max(1, round(int(n["servers"]) * scale)))
                total += int(n["servers"])
            w.writerow(n)
    shutil.copy(DATA / "edges.csv", dest / "edges.csv")
    shutil.copy(DATA / "domain.txt", dest / "domain.txt")
    return total


def write_fleet(path: Path, fleet: int, last: int) -> None:
    """The committed scenario's distribution, at a different size. Same seed, so
    the first 300 drivers of any fleet of 300 or more are the committed ones."""
    rng = random.Random(btn.SEED)
    with path.open("w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["id", "origin_id", "destination_id", "capacity", "level", "consumption",
                    "required_amount", "release_hour"])
        for k in range(fleet):
            worked = rng.uniform(0.0, btn.MAX_HOURS_ALREADY_WORKED)
            w.writerow([k + 1, 0, last, f"{btn.MAX_WORK_HOURS:.1f}", f"{btn.MAX_WORK_HOURS - worked:.3f}",
                        "1.0", "0.0", f"{rng.uniform(0.0, 24.0):.3f}"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / "data" / "truck-sweep.csv")
    args = parser.parse_args()
    evnet = find_evnet()
    last = len(read(DATA / "nodes.csv")) - 1

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        net = Path(tmp)
        for scale in SCALES:
            bays = write_scaled_network(net, scale)
            for fleet in FLEETS:
                demands = net / f"demands-{fleet}.csv"
                write_fleet(demands, fleet, last)
                out = subprocess.run([str(evnet), "compare", "--network", str(net), "--demands", str(demands),
                                      "--format", "csv"], capture_output=True, text=True, check=True).stdout
                for r in csv.DictReader(io.StringIO(out)):
                    rows.append({
                        "bay_scale": scale, "bays": bays, "fleet": fleet, "planner": r["planner"],
                        "completed": r["completed"], "stranded": r["stranded"], "turned_away": r["turned_away"],
                        "stranded_share": f"{int(r['stranded']) / fleet:.4f}",
                        "peak_utilisation": r["peak_utilisation"], "mean_elapsed_h": r["mean_elapsed_h"],
                    })
                s = {r["planner"]: r for r in rows if r["bay_scale"] == scale and r["fleet"] == fleet}
                print(f"  bays x{scale:<4} ({bays:3d})  fleet {fleet:5d}   stranded  " +
                      "  ".join(f"{p} {s[p]['stranded']:>4}" for p in s), flush=True)

    with args.out.open("w", newline="") as f:
        f.write("# Output of tools/truck_sweep.py on data/hume-trucks. Bays are an estimated input,\n"
                "# the fleet a scenario; read these as sensitivities, not forecasts.\n")
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()), lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    shown = args.out.resolve()
    print(f"wrote {shown.relative_to(ROOT) if shown.is_relative_to(ROOT) else shown}: {len(rows)} rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
