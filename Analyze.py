import argparse
import glob
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from scipy import stats
from scipy.sparse import csr_matrix
from scipy.sparse.csgraph import connected_components
from scipy.spatial.distance import cdist

RANGE_M = 100.0
PROTOS = ["aodv", "olsr", "dsdv"]
STYLE = {"aodv": ("tab:blue", "o"), "olsr": ("tab:orange", "s"), "dsdv": ("tab:green", "^")}
KPIS = [  # column, axis label
    ("pdr", "PDR (%)"),
    ("delay_mean", "Mean end-to-end delay (ms)"),
    ("delay_p95", "p95 end-to-end delay (ms)"),
    ("jitter", "Jitter (ms)"),
    ("nrl", "Normalized routing load (ctrl pkts / delivered pkt)"),
    ("hops", "Average hop count"),
]


def flowmon_xml_to_csv(xml_path, csv_path):
    """FlowMonitor XML -> CSV (Phase P1 requirement). Returns the DataFrame."""
    import xml.etree.ElementTree as ET
    root = ET.parse(xml_path).getroot()
    rows = []
    for fl in root.find("FlowStats"):
        r = {k: float(v.rstrip("ns+")) if k.startswith(("delay", "jitter", "time")) else int(v)
             for k, v in fl.attrib.items() if k != "flowId" or True}
        rows.append(r)
    df = pd.DataFrame(rows)
    df.to_csv(csv_path, index=False)
    return df


def connectivity(d, warm):
    pos = pd.read_csv(f"{d}/pos.csv")
    flows = pd.read_csv(f"{d}/flows.csv")
    reach, largest = [], []
    for t, g in pos[pos.t >= warm].groupby("t"):
        g = g.sort_values("node")
        xy = g[["x", "y"]].to_numpy()
        adj = csr_matrix(cdist(xy, xy) <= RANGE_M)
        _, lab = connected_components(adj, directed=False)
        reach += [lab[s] == lab[dd] for s, dd in zip(flows.src, flows.dst)]
        largest.append(np.bincount(lab).max() / len(lab))
    return 100 * np.mean(reach), 100 * np.mean(largest)


def run_kpis(d):
    s = pd.read_csv(f"{d}/summary.csv").iloc[0]
    warm = s.warmup
    tx = pd.read_csv(f"{d}/tx.csv")
    rx = pd.read_csv(f"{d}/rx.csv")
    ctrl = pd.read_csv(f"{d}/ctrl.csv")
    tx = tx[tx.t_tx >= warm]
    rx = rx[rx.t_tx >= warm].copy()
    rx["d"] = (rx.t_rx - rx.t_tx) * 1000.0
    window = s.send_stop - warm
    delivered = len(rx)
    jit = rx.sort_values(["flow", "seq"]).groupby("flow")["d"].diff().abs().mean()
    reach, largest = connectivity(d, warm)
    return dict(
        protocol=s.protocol, nodes=int(s.nodes), speed=float(s.speed), seed=int(s.seed),
        sent=len(tx), delivered=delivered,
        pdr=100.0 * delivered / len(tx) if len(tx) else np.nan,
        delay_mean=rx.d.mean() if delivered else np.nan,
        delay_p95=rx.d.quantile(0.95) if delivered else np.nan,
        jitter=jit,
        nrl=ctrl.ctrl_pkts.sum() / delivered if delivered else np.nan,
        ctrl_pkts=int(ctrl.ctrl_pkts.sum()),
        hops=rx.hops[rx.hops > 0].mean() if delivered else np.nan,
        thr_kbps=delivered * s.payload_bytes * 8 / window / s.flows / 1000.0,
        reach=reach, largest_cc=largest,
        offered_kbps_total=s.flows * s.rate_pps * s.payload_bytes * 8 / 1000.0,
        window_s=window, wall_s=s.wall_s,
    )


def ci95(x):
    x = x.dropna()
    return stats.t.ppf(0.975, len(x) - 1) * x.std(ddof=1) / np.sqrt(len(x)) if len(x) > 1 else np.nan


def aggregate(runs):
    cols = [k for k, _ in KPIS] + ["thr_kbps", "reach", "largest_cc"]
    out = []
    for (p, n, v), g in runs.groupby(["protocol", "nodes", "speed"]):
        r = dict(protocol=p, nodes=n, speed=v, n_seeds=len(g), sent_per_run=g.sent.mean(),
                 offered_kbps_total=g.offered_kbps_total.iloc[0], window_s=g.window_s.iloc[0],
                 wall_s_mean=g.wall_s.mean())
        for c in cols:
            r[c] = g[c].mean()
            r[c + "_ci"] = ci95(g[c])
        out.append(r)
    return pd.DataFrame(out)


def sweep_figure(agg, sweep, path):
    if sweep == "density":
        d = agg[agg.speed == 0]
        xcol, xlabel = "nodes", "Number of nodes (500 m x 500 m, static)"
    else:
        d = agg[(agg.nodes == 50)]
        xcol, xlabel = "speed", "Speed (m/s), 50 nodes"
    if d.empty:
        return
    fig, axes = plt.subplots(2, 3, figsize=(14, 7.5))
    for ax, (k, lab) in zip(axes.ravel(), KPIS):
        for p in PROTOS:
            g = d[d.protocol == p].sort_values(xcol)
            if g.empty:
                continue
            c, m = STYLE[p]
            ax.errorbar(g[xcol], g[k], yerr=g[k + "_ci"].fillna(0), color=c, marker=m,
                        capsize=3, label=p.upper())
        ax.set_xlabel(xlabel)
        ax.set_ylabel(lab)
        ax.set_xticks(sorted(d[xcol].unique()))
        ax.grid(alpha=0.3)
    axes[0, 0].legend()
    fig.suptitle(f"{sweep.capitalize()} sweep - mean and 95 % CI over seeds")
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def baseline_figure(runs, path):
    b = runs[(runs.nodes == 50) & (runs.speed == 0)]
    b = b[b.seed == b.seed.min()].set_index("protocol").reindex(PROTOS).dropna(subset=["pdr"])
    if b.empty:
        return
    fig, ax = plt.subplots(1, 2, figsize=(9, 3.8))
    ax[0].bar([p.upper() for p in b.index], b.pdr, color=[STYLE[p][0] for p in b.index])
    ax[0].set_ylabel("PDR (%)")
    ax[0].set_ylim(0, 105)
    x = np.arange(len(b))
    ax[1].bar(x - 0.2, b.delay_mean, 0.4, label="mean", color="tab:gray")
    ax[1].bar(x + 0.2, b.delay_p95, 0.4, label="p95", color="tab:red")
    ax[1].set_xticks(x, [p.upper() for p in b.index])
    ax[1].set_ylabel("End-to-end delay (ms)")
    ax[1].legend()
    fig.suptitle(f"Baseline: 50 nodes, static, seed {int(b.seed.iloc[0])}")
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", default=os.path.join(here, "results", "raw"))
    ap.add_argument("--out", default=os.path.join(here, "results"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    dirs = sorted(d for d in glob.glob(os.path.join(a.raw, "*")) if os.path.exists(d + "/.done"))
    if not dirs:
        raise SystemExit(f"no finished runs in {a.raw}")
    runs = pd.DataFrame([run_kpis(d) for d in dirs])
    runs.to_csv(os.path.join(a.out, "runs.csv"), index=False)
    agg = aggregate(runs)
    agg.to_csv(os.path.join(a.out, "agg.csv"), index=False)

    # sanity numbers next to the results (offered load, duration, samples) + partition report
    sanity = agg[["protocol", "nodes", "speed", "n_seeds", "offered_kbps_total", "window_s",
                  "sent_per_run", "reach", "largest_cc", "wall_s_mean"]]
    sanity.to_csv(os.path.join(a.out, "sanity.csv"), index=False)

    sweep_figure(agg, "density", os.path.join(a.out, "fig_density.png"))
    sweep_figure(agg, "speed", os.path.join(a.out, "fig_speed.png"))
    baseline_figure(runs, os.path.join(a.out, "fig_baseline.png"))

    part = agg[agg.reach < 99.0][["protocol", "nodes", "speed", "reach", "largest_cc"]]
    print(f"{len(runs)} runs analysed -> {a.out}")
    if len(part):
        print("Partitioned scenarios (src-dst connectivity < 99 %) - report, do not drop:")
        print(part.drop_duplicates(["nodes", "speed"]).to_string(index=False))
    print(agg[["protocol", "nodes", "speed", "n_seeds", "pdr", "pdr_ci", "delay_mean",
               "delay_p95", "nrl"]].round(2).to_string(index=False))


if __name__ == "__main__":
    main()
