#!/usr/bin/env python3
"""At what fleet size does the corridor actually congest, and does policy start to matter?

Stage 2 produced an uncomfortable result: once queues are allowed to drain, the
Hume corridor at its inherited fleet size is barely congested at all, and the
policies differ by cents. That is worth knowing -- it means the legacy project's
central claim, that queue balancing matters, was substantially an artefact of a
queue that never emptied.

But "no congestion at this fleet size" is not the same as "congestion never
matters". This sweep scales the fleet until the network saturates and records where
the policies begin to diverge, which is the honest version of the original claim.

Run:  python3 tools/saturation_sweep.py [--out results.csv]
"""

from __future__ import annotations

import argparse
import csv
import io
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EVNET = ROOT / "build" / "evnet"
BASE_DEMANDS = ROOT / "data" / "hume" / "demands.csv"
NETWORK = ROOT / "data" / "hume"

FLEET_SIZES = [200, 400, 800, 1600, 3200, 6400]
SEED = 20260817


def read_base() -> list[dict]:
    with BASE_DEMANDS.open() as fh:
        rows = [line for line in fh if not line.startswith("#")]
    return list(csv.DictReader(io.StringIO("".join(rows))))


def scaled_fleet(base: list[dict], size: int, rng: random.Random) -> list[dict]:
    """Resample the base fleet up to `size`, giving each vehicle a fresh id and a
    fresh departure time drawn from the same daily profile."""
    out = []
    for i in range(size):
        row = dict(base[i % len(base)])
        row["id"] = str(1_000_000 + i)
        # Same twin-peaked day as the generator, kept local to avoid importing it.
        roll = rng.random()
        if roll < 0.45:
            hour = rng.gauss(7.5, 1.5)
        elif roll < 0.75:
            hour = rng.gauss(14.0, 2.0)
        else:
            hour = rng.uniform(0.0, 16.0)
        row["release_hour"] = f"{min(max(hour, 0.0), 16.0):.2f}"
        out.append(row)
    return out


def run_compare(demands_path: Path) -> list[dict]:
    result = subprocess.run(
        [str(EVNET), "compare", "--network", str(NETWORK), "--demands", str(demands_path),
         "--engine", "events", "--format", "csv"],
        capture_output=True, text=True, check=True,
    )
    return list(csv.DictReader(io.StringIO(result.stdout)))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=None, help="write full results here")
    args = parser.parse_args()

    if not EVNET.exists():
        print(f"error: {EVNET} not found -- build the project first", file=sys.stderr)
        return 1

    base = read_base()
    rng = random.Random(SEED)
    rows: list[dict] = []

    header = f"{'fleet':>7} {'planner':<13} {'stranded':>9} {'mean wait':>10} {'p95 wait':>10} {'peak util':>10} {'mean gen $':>11}"
    print(header)
    print("-" * len(header))

    for size in FLEET_SIZES:
        fleet = scaled_fleet(base, size, rng)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "demands.csv"
            with path.open("w", newline="") as fh:
                writer = csv.DictWriter(fh, fieldnames=list(fleet[0].keys()))
                writer.writeheader()
                writer.writerows(fleet)
            summaries = run_compare(path)

        for summary in summaries:
            summary["fleet"] = size
            rows.append(summary)
            print(f"{size:>7} {summary['planner']:<13} {summary['stranded']:>9} "
                  f"{float(summary['mean_wait_h']):>9.2f}h {float(summary['p95_wait_h']):>9.2f}h "
                  f"{float(summary['peak_utilisation']) * 100:>9.0f}% "
                  f"{float(summary['mean_generalised']):>10.2f}")
        print()

    # Where does the choice of policy start to be worth anything?
    print("Spread between best and worst planner, by fleet size:")
    print(f"{'fleet':>7} {'gen $ spread':>13} {'wait spread':>13} {'best':<13} {'worst':<13}")
    print("-" * 62)
    for size in FLEET_SIZES:
        group = [r for r in rows if r["fleet"] == size]
        best = min(group, key=lambda r: float(r["mean_generalised"]))
        worst = max(group, key=lambda r: float(r["mean_generalised"]))
        gen_spread = float(worst["mean_generalised"]) - float(best["mean_generalised"])
        wait_spread = max(float(r["mean_wait_h"]) for r in group) - min(
            float(r["mean_wait_h"]) for r in group)
        print(f"{size:>7} {gen_spread:>12.2f} {wait_spread:>12.2f}h "
              f"{best['planner']:<13} {worst['planner']:<13}")

    if args.out:
        with args.out.open("w", newline="") as fh:
            writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
