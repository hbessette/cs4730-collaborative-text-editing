#!/usr/bin/env python3
"""Scalability analysis for p2p-editor.

Reads results produced by `run_eval.sh --eval scalability` and reports:
  - Convergence time (ms): first LATENCY_SEND → last LATENCY_APPLY across all peers
  - Per-peer throughput (ops/sec delivered): ops received per second per receiver peer
  - Average CPU usage (%): averaged over all peer processes

Outputs:
  - Summary table to stdout
  - CSV file (--csv)
  - Line chart PNG (--chart; requires matplotlib)

Usage:
    python3 scripts/analyze_scalability.py logs/scalability/ [--csv FILE] [--chart FILE]
"""

import csv
import math
import os
import re
import sys

# ---------------------------------------------------------------------------
# Log parsing
# ---------------------------------------------------------------------------

_SEND_RE  = re.compile(r"LATENCY_SEND\s+siteID=(\d+)\s+clock=(\d+)\s+ts_us=(\d+)")
_APPLY_RE = re.compile(r"LATENCY_APPLY\s+siteID=(\d+)\s+clock=(\d+)\s+ts_us=(\d+)")
_CPU_RE   = re.compile(r"cpu_pct=(\d+)%")


def _parse_log(path):
    sends = {}
    applies = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                sm = _SEND_RE.search(line)
                if sm:
                    key = (int(sm.group(1)), int(sm.group(2)))
                    sends[key] = int(sm.group(3))
                    continue
                am = _APPLY_RE.search(line)
                if am:
                    key = (int(am.group(1)), int(am.group(2)))
                    ts = int(am.group(3))
                    # Keep the LATEST apply timestamp across all receiver logs
                    # (max = last peer to apply = true convergence point).
                    if key not in applies or ts > applies[key]:
                        applies[key] = ts
    except OSError:
        pass
    return sends, applies


def _load_subdir(subdir):
    """Merge all peer_*.log files; return (sends, max_applies) dicts."""
    all_sends = {}
    all_max_applies = {}
    try:
        entries = sorted(os.listdir(subdir))
    except OSError:
        return all_sends, all_max_applies

    for fname in entries:
        if not (fname.startswith("peer_") and fname.endswith(".log")):
            continue
        sends, applies = _parse_log(os.path.join(subdir, fname))
        all_sends.update(sends)
        for key, ts in applies.items():
            if key not in all_max_applies or ts > all_max_applies[key]:
                all_max_applies[key] = ts

    return all_sends, all_max_applies


def _parse_cpu(subdir):
    """Return list of CPU percentages from cpu_N.txt files."""
    cpu_values = []
    try:
        for fname in sorted(os.listdir(subdir)):
            if not (fname.startswith("cpu_") and fname.endswith(".txt")):
                continue
            fpath = os.path.join(subdir, fname)
            try:
                with open(fpath, encoding="utf-8", errors="replace") as fh:
                    content = fh.read()
                m = _CPU_RE.search(content)
                if m:
                    cpu_values.append(int(m.group(1)))
            except OSError:
                pass
    except OSError:
        pass
    return cpu_values


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------

def compute_metrics(subdir, n_peers):
    """Compute scalability metrics for one n-peer experiment directory."""
    sends, max_applies = _load_subdir(subdir)
    cpu_list = _parse_cpu(subdir)

    if not sends:
        return None

    # Match each sent op to its latest apply timestamp.
    matched_apply_ts = [max_applies[k] for k in sends if k in max_applies]
    dropped = len(sends) - len(matched_apply_ts)

    if not matched_apply_ts:
        return None

    first_send_us  = min(sends.values())
    last_apply_us  = max(matched_apply_ts)
    convergence_ms = (last_apply_us - first_send_us) / 1000.0
    convergence_sec = convergence_ms / 1000.0

    total_sent = len(sends)
    delivered  = len(matched_apply_ts)

    # System throughput: unique ops delivered per second across all peers.
    sys_throughput = delivered / convergence_sec if convergence_sec > 0 else 0.0

    # Per-peer throughput: ops received per second by a single receiver.
    # Each peer receives ops from (n_peers-1) senders.  Divide system
    # throughput by n_peers to get the per-peer share.
    per_peer_tput = sys_throughput / n_peers if n_peers > 0 else 0.0

    avg_cpu = sum(cpu_list) / len(cpu_list) if cpu_list else float("nan")

    return {
        "n_peers":           n_peers,
        "total_sent":        total_sent,
        "delivered":         delivered,
        "dropped":           dropped,
        "drop_pct":          100.0 * dropped / total_sent if total_sent > 0 else 0.0,
        "convergence_ms":    convergence_ms,
        "sys_throughput":    sys_throughput,
        "per_peer_tput":     per_peer_tput,
        "avg_cpu_pct":       avg_cpu,
        "n_cpu_samples":     len(cpu_list),
    }


# ---------------------------------------------------------------------------
# Bottleneck detection
# ---------------------------------------------------------------------------

def detect_bottleneck(results):
    """
    Flag a bottleneck if convergence time grows super-linearly (≥2× faster
    than peer count) or average CPU exceeds 80%.

    Returns a list of (n_peers, reason) tuples.
    """
    flags = []
    prev = None
    for r in results:
        if prev is not None:
            ratio_peers = r["n_peers"] / prev["n_peers"]
            ratio_conv  = r["convergence_ms"] / prev["convergence_ms"] \
                          if prev["convergence_ms"] > 0 else 0
            if ratio_conv >= 2 * ratio_peers:
                flags.append((r["n_peers"],
                               f"convergence time grew {ratio_conv:.1f}× "
                               f"while peers grew {ratio_peers:.1f}×"))
        cpu = r["avg_cpu_pct"]
        if not math.isnan(cpu) and cpu >= 80:
            flags.append((r["n_peers"], f"avg CPU {cpu:.0f}% ≥ 80%"))
        prev = r
    return flags


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def _fmt(v, decimals=1):
    if isinstance(v, float) and math.isnan(v):
        return "N/A"
    return f"{v:.{decimals}f}"


def print_table(results):
    hdr = (
        f"  {'Peers':>5}  {'Sent':>6}  {'Rcvd':>6}  {'Drop%':>6}"
        f"  {'Conv ms':>8}  {'Sys tput':>10}  {'Per-peer tput':>14}  {'CPU%':>6}"
    )
    print()
    print("=" * 80)
    print("  P2P Editor — Scalability Results")
    print("=" * 80)
    print(hdr)
    print("  " + "-" * 76)
    for r in results:
        print(
            f"  {r['n_peers']:>5}  {r['total_sent']:>6}  {r['delivered']:>6}"
            f"  {_fmt(r['drop_pct']):>6}"
            f"  {_fmt(r['convergence_ms']):>8}"
            f"  {_fmt(r['sys_throughput']):>10}"
            f"  {_fmt(r['per_peer_tput']):>14}"
            f"  {_fmt(r['avg_cpu_pct']):>6}"
        )
    print()
    print("  Columns:")
    print("    Conv ms      — wall time from first send to last apply (ms)")
    print("    Sys tput     — total unique ops converged per second (ops/s)")
    print("    Per-peer tput— sys_tput / n_peers (ops/s per peer)")
    print("    CPU%         — average CPU% per peer process (/usr/bin/time)")
    print()


def print_bottleneck(flags):
    if not flags:
        print("  No bottleneck detected.")
    else:
        print("  Bottleneck indicators:")
        for n, reason in flags:
            print(f"    {n} peers : {reason}")
    print()


def write_csv(results, path):
    fieldnames = [
        "n_peers", "total_sent", "delivered", "drop_pct",
        "convergence_ms", "sys_throughput_ops_sec",
        "per_peer_throughput_ops_sec", "avg_cpu_pct",
    ]
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=fieldnames)
        w.writeheader()
        for r in results:
            w.writerow({
                "n_peers":                       r["n_peers"],
                "total_sent":                    r["total_sent"],
                "delivered":                     r["delivered"],
                "drop_pct":                      f"{r['drop_pct']:.2f}",
                "convergence_ms":                f"{r['convergence_ms']:.1f}",
                "sys_throughput_ops_sec":        f"{r['sys_throughput']:.1f}",
                "per_peer_throughput_ops_sec":   f"{r['per_peer_tput']:.1f}",
                "avg_cpu_pct":                   _fmt(r["avg_cpu_pct"]),
            })
    print(f"CSV written to: {path}")


def write_chart(results, path):
    """Generate a 3-panel line chart (convergence time, throughput, CPU)."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available — skipping chart (install with: pip install matplotlib)")
        _print_ascii_chart(results)
        return

    peers   = [r["n_peers"]        for r in results]
    conv    = [r["convergence_ms"] for r in results]
    tput    = [r["per_peer_tput"]  for r in results]
    cpu_raw = [r["avg_cpu_pct"]    for r in results]
    cpu     = [v if not math.isnan(v) else None for v in cpu_raw]

    fig, axes = plt.subplots(1, 3, figsize=(13, 4))
    fig.suptitle("P2P Editor — Scalability (100 ops/peer @ 5 ops/sec)", fontsize=13)

    # Panel 1: Convergence time
    ax = axes[0]
    ax.plot(peers, conv, marker="o", color="steelblue", linewidth=2)
    ax.set_title("Convergence Time")
    ax.set_xlabel("Peers")
    ax.set_ylabel("ms")
    ax.set_xticks(peers)
    ax.grid(True, linestyle="--", alpha=0.5)

    # Panel 2: Per-peer throughput
    ax = axes[1]
    ax.plot(peers, tput, marker="s", color="darkorange", linewidth=2)
    ax.set_title("Per-Peer Throughput")
    ax.set_xlabel("Peers")
    ax.set_ylabel("ops/sec delivered")
    ax.set_xticks(peers)
    ax.grid(True, linestyle="--", alpha=0.5)

    # Panel 3: CPU usage
    ax = axes[2]
    cpu_y = [v for v in cpu if v is not None]
    cpu_x = [peers[i] for i, v in enumerate(cpu) if v is not None]
    if cpu_y:
        ax.plot(cpu_x, cpu_y, marker="^", color="firebrick", linewidth=2)
        ax.axhline(80, color="gray", linestyle="--", linewidth=1, label="80% threshold")
        ax.legend(fontsize=8)
    else:
        ax.text(0.5, 0.5, "No CPU data\n(cpu_N.txt missing)", ha="center",
                va="center", transform=ax.transAxes, color="gray")
    ax.set_title("Avg CPU per Peer")
    ax.set_xlabel("Peers")
    ax.set_ylabel("CPU %")
    ax.set_xticks(peers)
    ax.set_ylim(0, 105)
    ax.grid(True, linestyle="--", alpha=0.5)

    fig.tight_layout()
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"Chart saved to: {path}")


def _print_ascii_chart(results):
    """Simple ASCII bar chart fallback when matplotlib is unavailable."""
    print()
    print("  Convergence time (ms) — ASCII chart")
    print("  " + "-" * 40)
    max_conv = max(r["convergence_ms"] for r in results) or 1
    for r in results:
        bar_len = int(r["convergence_ms"] / max_conv * 30)
        bar = "█" * bar_len
        print(f"  {r['n_peers']:>3} peers │{bar:<30}│ {r['convergence_ms']:.0f} ms")
    print()
    print("  Per-peer throughput (ops/sec)")
    print("  " + "-" * 40)
    max_tput = max(r["per_peer_tput"] for r in results) or 1
    for r in results:
        bar_len = int(r["per_peer_tput"] / max_tput * 30)
        bar = "█" * bar_len
        print(f"  {r['n_peers']:>3} peers │{bar:<30}│ {r['per_peer_tput']:.1f} ops/s")
    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

_NPEER_RE = re.compile(r"^(\d+)peer$")


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        sys.exit(0 if len(sys.argv) > 1 else 1)

    results_dir = sys.argv[1]
    csv_out   = None
    chart_out = None

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

    # Discover n-peer subdirectories in numeric order.
    subdirs = []
    try:
        def _peer_sort_key(name):
            mo = _NPEER_RE.match(name)
            return int(mo.group(1)) if mo else 0

        for entry in sorted(os.listdir(results_dir), key=_peer_sort_key):
            mo = _NPEER_RE.match(entry)
            if not mo:
                continue
            n = int(mo.group(1))
            subdirs.append((n, os.path.join(results_dir, entry)))
    except OSError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        sys.exit(1)

    if not subdirs:
        print(f"No N-peer subdirectories found under '{results_dir}'")
        sys.exit(1)

    results = []
    for n, subdir in subdirs:
        r = compute_metrics(subdir, n)
        if r is None:
            print(f"Warning: no LATENCY data found in {subdir} — skipping")
            continue
        results.append(r)

    if not results:
        print("No usable experiment data found.")
        sys.exit(1)

    print_table(results)

    flags = detect_bottleneck(results)
    print("  Bottleneck analysis:")
    print_bottleneck(flags)

    if csv_out:
        write_csv(results, csv_out)

    # Default chart path if not specified.
    if chart_out is None:
        chart_out = os.path.join(results_dir, "scalability_chart.png")
    write_chart(results, chart_out)


if __name__ == "__main__":
    main()
