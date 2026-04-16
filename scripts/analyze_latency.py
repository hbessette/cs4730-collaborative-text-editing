#!/usr/bin/env python3
"""Latency analysis for p2p-editor.

Reads results produced by `run_eval.sh --eval latency` and reports per-config
end-to-end operation propagation latency.

Usage:
    python3 scripts/analyze_latency.py <results-dir> [--csv FILE] [--chart FILE]

Discovers all direct subdirectories of <results-dir> that contain peer_*.log
files with LATENCY_SEND / LATENCY_APPLY records (e.g. 2peer/, 5peer/).
Skips convergence/ and scalability/ subdirs.

Output:
  - Summary table (stdout)
  - CSV file (--csv)
  - Bar chart PNG with a panel per config (--chart; requires matplotlib)
"""

import csv
import math
import os
import re
import sys

# ---------------------------------------------------------------------------
# Log parsing (shared with analyze_results.py)
# ---------------------------------------------------------------------------

_SEND_RE     = re.compile(r"LATENCY_SEND\s+siteID=(\d+)\s+clock=(\d+)\s+ts_us=(\d+)")
_APPLY_RE    = re.compile(r"LATENCY_APPLY\s+siteID=(\d+)\s+clock=(\d+)\s+ts_us=(\d+)")
_SEND_MS_RE  = re.compile(r"LATENCY_SEND\s+siteID=(\d+)\s+clock=(\d+)\s+ts_ms=(\d+)")
_APPLY_MS_RE = re.compile(r"LATENCY_APPLY\s+siteID=(\d+)\s+clock=(\d+)\s+ts_ms=(\d+)")

_SKIP_DIRS = {"convergence", "scalability"}


def _parse_log(path):
    sends = {}
    applies = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                sm = _SEND_RE.search(line)
                if sm:
                    sends[(int(sm.group(1)), int(sm.group(2)))] = int(sm.group(3))
                    continue
                am = _APPLY_RE.search(line)
                if am:
                    applies[(int(am.group(1)), int(am.group(2)))] = int(am.group(3))
                    continue
                sm2 = _SEND_MS_RE.search(line)
                if sm2:
                    sends[(int(sm2.group(1)), int(sm2.group(2)))] = int(sm2.group(3)) * 1000
                    continue
                am2 = _APPLY_MS_RE.search(line)
                if am2:
                    applies[(int(am2.group(1)), int(am2.group(2)))] = int(am2.group(3)) * 1000
    except OSError:
        pass
    return sends, applies


def _load_subdir(subdir):
    """Merge all *.log files; keep the earliest apply timestamp per op."""
    all_sends = {}
    all_applies = {}
    try:
        for fname in sorted(os.listdir(subdir)):
            if not fname.endswith(".log"):
                continue
            s, a = _parse_log(os.path.join(subdir, fname))
            all_sends.update(s)
            for k, ts in a.items():
                if k not in all_applies or ts < all_applies[k]:
                    all_applies[k] = ts
    except OSError:
        pass
    return all_sends, all_applies


def _percentile(data, p):
    if not data:
        return float("nan")
    s = sorted(data)
    return s[max(0, int(len(s) * p / 100.0) - 1)]


# ---------------------------------------------------------------------------
# Per-subdir analysis
# ---------------------------------------------------------------------------

def analyze_subdir(subdir, label):
    sends, applies = _load_subdir(subdir)
    latencies_us = []
    dropped = 0
    negative = 0

    for key, send_us in sends.items():
        if key in applies:
            delta = applies[key] - send_us
            if delta >= 0:
                latencies_us.append(float(delta))
            else:
                negative += 1
        else:
            dropped += 1

    total = len(sends)
    n_peers = 0
    m = re.search(r"(\d+)peer", label)
    if m:
        n_peers = int(m.group(1))

    return {
        "label":     label,
        "n_peers":   n_peers,
        "total":     total,
        "delivered": len(latencies_us),
        "dropped":   dropped,
        "negative":  negative,
        "drop_pct":  100.0 * dropped / total if total > 0 else 0.0,
        "min":       min(latencies_us) if latencies_us else float("nan"),
        "avg":       sum(latencies_us) / len(latencies_us) if latencies_us else float("nan"),
        "p50":       _percentile(latencies_us, 50),
        "p95":       _percentile(latencies_us, 95),
        "p99":       _percentile(latencies_us, 99),
        "max":       max(latencies_us) if latencies_us else float("nan"),
        "latencies": latencies_us,
    }


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def _fmt(v):
    return f"{v:.1f}" if not (isinstance(v, float) and math.isnan(v)) else "N/A"


def print_table(results):
    print()
    print("=" * 108)
    print("  P2P Editor — Latency Results")
    print("=" * 108)
    hdr = (
        f"  {'Config':<22} {'Peers':>5} {'Sent':>6} {'Rcvd':>6}"
        f" {'Min µs':>10} {'Avg µs':>10} {'P50 µs':>10} {'P95 µs':>10} {'P99 µs':>10}"
        f" {'Max µs':>10} {'Drop%':>7}"
    )
    print(hdr)
    print("  " + "-" * 104)
    for r in results:
        print(
            f"  {r['label']:<22} {r['n_peers']:>5} {r['total']:>6} {r['delivered']:>6}"
            f" {_fmt(r['min']):>10} {_fmt(r['avg']):>10} {_fmt(r['p50']):>10}"
            f" {_fmt(r['p95']):>10} {_fmt(r['p99']):>10} {_fmt(r['max']):>10}"
            f" {r['drop_pct']:>6.1f}%"
        )
        if r["drop_pct"] > 1.0:
            print(f"  *** WARNING: drop rate {r['drop_pct']:.1f}% > 1%")
        if r["negative"] > 0:
            print(f"  Note: {r['negative']} sample(s) excluded (negative delta — NTP skew)")
    print()


def write_csv(results, path):
    fields = ["config", "n_peers", "total_ops", "delivered", "dropped",
              "drop_pct", "min_us", "avg_us", "p50_us", "p95_us", "p99_us", "max_us"]
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        for r in results:
            w.writerow({
                "config":    r["label"],
                "n_peers":   r["n_peers"],
                "total_ops": r["total"],
                "delivered": r["delivered"],
                "dropped":   r["dropped"],
                "drop_pct":  f"{r['drop_pct']:.2f}",
                "min_us":    _fmt(r["min"]),
                "avg_us":    _fmt(r["avg"]),
                "p50_us":    _fmt(r["p50"]),
                "p95_us":    _fmt(r["p95"]),
                "p99_us":    _fmt(r["p99"]),
                "max_us":    _fmt(r["max"]),
            })
    print(f"CSV written to: {path}")


def write_chart(results, path):
    """Bar chart: P50/P95/P99 grouped by config, one group per config."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print(
            "matplotlib not available — skipping latency chart "
            "(install with: pip install matplotlib)"
        )
        _print_ascii_chart(results)
        return

    def _v(r, key):
        v = r[key]
        return v if not math.isnan(v) else 0

    labels = [r["label"] for r in results]
    mins = [_v(r, "min") for r in results]
    avgs = [_v(r, "avg") for r in results]
    p50  = [_v(r, "p50") for r in results]
    p95  = [_v(r, "p95") for r in results]
    p99  = [_v(r, "p99") for r in results]
    maxs = [_v(r, "max") for r in results]

    x = np.arange(len(labels))
    width = 0.13

    fig, ax = plt.subplots(figsize=(max(7, len(labels) * 3), 5))
    ax.bar(x - 5 * width / 2, mins, width, label="Min",  color="mediumseagreen")
    ax.bar(x - 3 * width / 2, avgs, width, label="Avg",  color="steelblue")
    ax.bar(x - 1 * width / 2, p50,  width, label="P50",  color="cornflowerblue")
    ax.bar(x + 1 * width / 2, p95,  width, label="P95",  color="darkorange")
    ax.bar(x + 3 * width / 2, p99,  width, label="P99",  color="firebrick")
    ax.bar(x + 5 * width / 2, maxs, width, label="Max",  color="darkred")

    ax.set_title("P2P Editor — End-to-End Op Latency")
    ax.set_xlabel("Configuration")
    ax.set_ylabel("Latency (µs)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.legend()
    ax.grid(axis="y", linestyle="--", alpha=0.5)

    fig.tight_layout()
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"Latency chart saved to: {path}")


def _print_ascii_chart(results):
    print()
    print("  Avg Latency (µs) — ASCII chart")
    print("  " + "-" * 42)
    max_val = max((r["avg"] for r in results if not math.isnan(r["avg"])), default=1) or 1
    for r in results:
        v = r["avg"] if not math.isnan(r["avg"]) else 0
        bar = "█" * int(v / max_val * 30)
        print(
            f"  {r['label']:<16} │{bar:<30}│"
            f" min={_fmt(r['min'])} avg={_fmt(r['avg'])} max={_fmt(r['max'])} µs"
        )
    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        sys.exit(0 if len(sys.argv) > 1 else 1)

    results_dir = sys.argv[1]
    csv_out = chart_out = None

    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == "--csv" and i + 1 < len(sys.argv):
            csv_out = sys.argv[i + 1]; i += 2
        elif sys.argv[i] == "--chart" and i + 1 < len(sys.argv):
            chart_out = sys.argv[i + 1]; i += 2
        else:
            print(f"ERROR: unknown argument '{sys.argv[i]}'", file=sys.stderr)
            sys.exit(1)

    if not os.path.isdir(results_dir):
        print(f"ERROR: directory not found: {results_dir}", file=sys.stderr)
        sys.exit(1)

    results = []
    try:
        for entry in sorted(os.listdir(results_dir)):
            if entry.startswith(".") or entry in _SKIP_DIRS:
                continue
            subdir = os.path.join(results_dir, entry)
            if not os.path.isdir(subdir):
                continue
            r = analyze_subdir(subdir, entry)
            if r["total"] == 0:
                continue
            results.append(r)
    except OSError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)

    if not results:
        print(f"No latency data found under '{results_dir}'")
        sys.exit(1)

    print_table(results)

    if csv_out:
        write_csv(results, csv_out)

    chart_path = chart_out or os.path.join(results_dir, "latency_chart.png")
    write_chart(results, chart_path)


if __name__ == "__main__":
    main()
