#!/usr/bin/env python3
"""Run the whole Project 3 matrix (or just the midterm baseline) in parallel.

Core matrix: 3 protocols x 5 points x 5 seeds = 75 runs
  points = density sweep {25,50,100} nodes @ 0 m/s  +  speed sweep {1,10} m/s @ 50 nodes

Usage:
  python3 run_matrix.py --ns3-dir ~/ns-3.xx --install --build --jobs 4     # full matrix
  python3 run_matrix.py --ns3-dir ~/ns-3.xx --baseline                     # midterm bar
Finished runs (marker file .done) are skipped, so an interrupted batch can be resumed.
"""
import argparse
import itertools
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
PROTOCOLS = ["aodv", "olsr", "dsdv"]
POINTS = [(25, 0), (50, 0), (100, 0), (50, 1), (50, 10)]  # (nodes, speed m/s)


def run_dir(raw, proto, n, v, seed):
    return os.path.join(raw, f"{proto}_n{n}_v{v}_s{seed}")


def run_one(ns3_dir, raw, proto, n, v, seed, sim_time, warmup):
    d = run_dir(raw, proto, n, v, seed)
    if os.path.exists(os.path.join(d, ".done")):
        return (proto, n, v, seed, "skipped", 0.0)
    os.makedirs(d, exist_ok=True)
    args = (f"manet-compare --protocol={proto} --nodes={n} --speed={v} --seed={seed} "
            f"--simTime={sim_time} --warmup={warmup} --outDir={d}")
    t0 = time.time()
    with open(os.path.join(d, "stdout.txt"), "w") as log:
        try:
            rc = subprocess.call([os.path.join(ns3_dir, "ns3"), "run", "--no-build", args],
                                 cwd=ns3_dir, stdout=log, stderr=subprocess.STDOUT)
        except OSError as e:
            log.write(str(e))
            rc = -1
    dt = time.time() - t0
    if rc == 0:
        open(os.path.join(d, ".done"), "w").close()
        return (proto, n, v, seed, "ok", dt)
    return (proto, n, v, seed, f"FAILED rc={rc} (see {d}/stdout.txt)", dt)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ns3-dir", required=True, help="ns-3 source tree (contains ./ns3)")
    ap.add_argument("--raw", default=os.path.join(HERE, "results", "raw"))
    ap.add_argument("--seeds", type=int, default=5)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2),
                    help="parallel runs; check RAM for 100-node runs")
    ap.add_argument("--sim-time", type=float, default=200.0)
    ap.add_argument("--warmup", type=float, default=20.0)
    ap.add_argument("--install", action="store_true", help="copy scratch/manet-compare.cc into ns-3")
    ap.add_argument("--build", action="store_true", help="run ./ns3 build once before the batch")
    ap.add_argument("--baseline", action="store_true",
                    help="midterm bar only: 50 nodes, static, 3 protocols, seed 1")
    a = ap.parse_args()

    ns3 = os.path.abspath(os.path.expanduser(a.ns3_dir))
    if a.install:
        shutil.copy(os.path.join(HERE, "scratch", "manet-compare.cc"),
                    os.path.join(ns3, "scratch", "manet-compare.cc"))
    if a.build:
        subprocess.check_call([os.path.join(ns3, "ns3"), "build"], cwd=ns3)

    points = [(50, 0)] if a.baseline else POINTS
    seeds = [1] if a.baseline else list(range(1, a.seeds + 1))
    # heaviest first so the long 100-node runs start early
    jobs = sorted(itertools.product(PROTOCOLS, points, seeds), key=lambda j: -j[1][0])
    raw = os.path.abspath(a.raw)
    os.makedirs(raw, exist_ok=True)
    print(f"{len(jobs)} runs, {a.jobs} in parallel -> {raw}")

    failed = []
    t0 = time.time()
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        futs = [ex.submit(run_one, ns3, raw, p, n, v, s, a.sim_time, a.warmup)
                for p, (n, v), s in jobs]
        for i, f in enumerate(as_completed(futs), 1):
            proto, n, v, s, status, dt = f.result()
            print(f"[{i:3d}/{len(jobs)}] {proto:5s} n={n:<3d} v={v:<2d} seed={s} "
                  f"{status} ({dt:.0f}s)", flush=True)
            if status.startswith("FAILED"):
                failed.append((proto, n, v, s))
    print(f"done in {(time.time() - t0) / 60:.1f} min, {len(failed)} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
