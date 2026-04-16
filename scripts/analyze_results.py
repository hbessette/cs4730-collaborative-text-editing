#!/usr/bin/env python3
"""Comprehensive analysis of p2p-editor evaluation results.

Usage:
    python3 scripts/analyze_results.py <results-dir> [--csv FILE] [--chart FILE]

Scans all subdirectories of <results-dir>.

Latency experiments (2peer, 5peer, concurrent_*):
  - Reads all *.log files for LATENCY_SEND and LATENCY_APPLY records.
  - Computes per-operation end-to-end latency in ms; excludes negatives.
  - Reports P50/P95/P99 and drop percentage.

Convergence experiments (convergence/trial_NNN/ structure):
  - Runs 50 trials; each trial dir holds peer_*_dump.txt files.
  - Byte-for-byte comparison across all peers in each trial.
  - Reports pass/fail counts; prints full divergence table.
  - Divergent trials include which peers differed and the trial seed.

Scalability experiments (scalability/Npeer/ structure):
  - Reads all peer_*.log files per N-peer config for LATENCY_SEND/APPLY.
  - Uses the LATEST apply timestamp to measure true convergence time.
  - Reads cpu_N.txt files for average CPU usage per peer.
  - Reports convergence time, per-peer throughput, CPU; flags bottlenecks.
  - Generates a line chart PNG (requires matplotlib; ASCII fallback otherwise).

CSV columns: config,n_peers,total_ops,delivered,drop_pct,
             min_us,avg_us,p50_us,p95_us,p99_us,max_us,
             converged,trials,trials_passed,trials_failed,
             convergence_ms,sys_throughput_ops_sec,per_peer_throughput_ops_sec,
             avg_cpu_pct
"""

import csv
import math
import os
import re
import sys

# ---------------------------------------------------------------------------
# Log-line patterns
# ---------------------------------------------------------------------------

_SEND_RE  = re.compile(r"LATENCY_SEND\s+siteID=(\d+)\s+clock=(\d+)\s+ts_us=(\d+)")
_APPLY_RE = re.compile(r"LATENCY_APPLY\s+siteID=(\d+)\s+clock=(\d+)\s+ts_us=(\d+)")

# Backward-compat: old format used ts_ms= (values already in ms).
_SEND_MS_RE  = re.compile(r"LATENCY_SEND\s+siteID=(\d+)\s+clock=(\d+)\s+ts_ms=(\d+)")
_APPLY_MS_RE = re.compile(r"LATENCY_APPLY\s+siteID=(\d+)\s+clock=(\d+)\s+ts_ms=(\d+)")

_CPU_RE = re.compile(r"cpu_pct=(\d+)%")


def parse_log(path):
    """Parse a single log file.

    Returns (sends, applies) where each is a dict:
        (siteID, clock) -> timestamp_us  (int, microseconds)

    For ts_ms= entries, the stored value is multiplied by 1000 to normalise
    to microseconds so downstream arithmetic is uniform.
    """
    sends = {}
    applies = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.rstrip()

                # ts_us= (preferred format)
                sm = _SEND_RE.search(line)
                if sm:
                    key = (int(sm.group(1)), int(sm.group(2)))
                    sends[key] = int(sm.group(3))
                    continue
                am = _APPLY_RE.search(line)
                if am:
                    key = (int(am.group(1)), int(am.group(2)))
                    applies[key] = int(am.group(3))
                    continue

                # ts_ms= (backward-compat — convert to us)
                sm2 = _SEND_MS_RE.search(line)
                if sm2:
                    key = (int(sm2.group(1)), int(sm2.group(2)))
                    sends[key] = int(sm2.group(3)) * 1000
                    continue
                am2 = _APPLY_MS_RE.search(line)
                if am2:
                    key = (int(am2.group(1)), int(am2.group(2)))
                    applies[key] = int(am2.group(3)) * 1000
    except FileNotFoundError:
        pass
    return sends, applies


def load_subdir(subdir_path):
    """Merge all *.log files in subdir_path into unified sends/applies dicts.

    For applies, keeps the *earliest* timestamp per (siteID, clock) pair
    across all receiver logs.
    """
    all_sends = {}
    all_applies = {}
    try:
        entries = sorted(os.listdir(subdir_path))
    except OSError:
        return all_sends, all_applies

    for fname in entries:
        if not fname.endswith(".log"):
            continue
        sends, applies = parse_log(os.path.join(subdir_path, fname))
        all_sends.update(sends)
        for key, ts in applies.items():
            if key not in all_applies or ts < all_applies[key]:
                all_applies[key] = ts

    return all_sends, all_applies


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def percentile(data, p):
    if not data:
        return float("nan")
    s = sorted(data)
    idx = max(0, int(len(s) * p / 100.0) - 1)
    return s[idx]


def analyze_subdir(subdir_path, label):
    """Return a result dict for one experiment subdirectory."""
    sends, applies = load_subdir(subdir_path)

    latencies_ms = []
    dropped = 0
    negative = 0

    latencies_us = []
    for key, send_ts_us in sends.items():
        if key in applies:
            delta_us = applies[key] - send_ts_us
            if delta_us >= 0:
                latencies_us.append(float(delta_us))
            else:
                negative += 1
        else:
            dropped += 1

    total = len(sends)
    drop_pct = dropped / total * 100 if total > 0 else 0.0

    lat_min = min(latencies_us) if latencies_us else float("nan")
    lat_avg = sum(latencies_us) / len(latencies_us) if latencies_us else float("nan")
    lat_max = max(latencies_us) if latencies_us else float("nan")
    latencies_ms = latencies_us  # keep name for percentile() calls below

    # Convergence check: read all *_dump.txt files.
    # Empty dumps (receiver-only nodes in latency eval) are excluded.
    dump_contents = []
    try:
        for fname in sorted(os.listdir(subdir_path)):
            if not fname.endswith("_dump.txt"):
                continue
            fpath = os.path.join(subdir_path, fname)
            try:
                with open(fpath, encoding="utf-8", errors="replace") as fh:
                    content = fh.read().strip()
                if content:
                    dump_contents.append(content)
            except OSError:
                pass
    except OSError:
        pass

    if len(dump_contents) < 2:
        converged = "N/A"
    elif all(d == dump_contents[0] for d in dump_contents):
        converged = "true"
    else:
        converged = "false"

    # Infer n_peers from label (e.g. "2peer" -> 2, "convergence_5peer" -> 5).
    n_peers = 0
    m = re.search(r"(\d+)peer", label)
    if m:
        n_peers = int(m.group(1))

    return {
        "label":      label,
        "n_peers":    n_peers,
        "total":      total,
        "delivered":  len(latencies_ms),
        "dropped":    dropped,
        "negative":   negative,
        "drop_pct":   drop_pct,
        "p50":        percentile(latencies_us, 50),
        "p95":        percentile(latencies_us, 95),
        "p99":        percentile(latencies_us, 99),
        "min":        lat_min,
        "avg":        lat_avg,
        "max":        lat_max,
        "converged":  converged,
        "latencies":  latencies_us,
    }


# ---------------------------------------------------------------------------
# Convergence trial analysis
# ---------------------------------------------------------------------------

_TRIAL_RE = re.compile(r"^trial_(\d+)$")
_META_RE  = re.compile(r"(\w+)=(\S+)")


def _read_meta(trial_dir):
    """Parse meta.txt into a dict; returns {} if absent."""
    meta = {}
    meta_path = os.path.join(trial_dir, "meta.txt")
    try:
        with open(meta_path, encoding="utf-8") as fh:
            for m in _META_RE.finditer(fh.read()):
                meta[m.group(1)] = m.group(2)
    except OSError:
        pass
    return meta


def _read_dump(path):
    """Return stripped dump content, or None if the file is missing/empty."""
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            content = fh.read().strip()
        return content if content else None
    except OSError:
        return None


def analyze_convergence_trials(convergence_dir):
    """Analyse all trial_NNN/ subdirs under convergence_dir.

    Returns a dict with aggregate stats and per-trial details.
    """
    trial_dirs = []
    try:
        for entry in sorted(os.listdir(convergence_dir)):
            if _TRIAL_RE.match(entry):
                trial_dirs.append(os.path.join(convergence_dir, entry))
    except OSError:
        pass

    if not trial_dirs:
        return None

    trials_passed = 0
    trials_failed = 0
    divergent = []   # list of dicts describing each failure

    for tdir in trial_dirs:
        meta = _read_meta(tdir)
        trial_num = int(meta.get("trial", 0))
        n_peers   = int(meta.get("n_peers", 0))
        seed      = meta.get("seed", "?")
        ops       = meta.get("ops", "?")

        # Collect all non-empty dump files.
        dumps = {}  # peer_index -> content
        for fname in sorted(os.listdir(tdir)):
            if not fname.endswith("_dump.txt"):
                continue
            m = re.match(r"peer_(\d+)_dump\.txt$", fname)
            if not m:
                continue
            content = _read_dump(os.path.join(tdir, fname))
            if content is not None:
                dumps[int(m.group(1))] = content

        if len(dumps) < 2:
            # Can't compare fewer than 2 peers — count as failure.
            trials_failed += 1
            divergent.append({
                "trial": trial_num,
                "n_peers": n_peers,
                "seed": seed,
                "ops": ops,
                "reason": f"only {len(dumps)} dump(s) collected (expected {n_peers})",
                "diffs": [],
            })
            continue

        reference = dumps[min(dumps)]
        diffs = [
            f"peer_{i} differs from peer_{min(dumps)}"
            for i, content in dumps.items()
            if content != reference and i != min(dumps)
        ]

        if not diffs:
            trials_passed += 1
        else:
            trials_failed += 1
            divergent.append({
                "trial": trial_num,
                "n_peers": n_peers,
                "seed": seed,
                "ops": ops,
                "reason": f"{len(diffs)} peer(s) diverged",
                "diffs": diffs,
            })

    total = trials_passed + trials_failed
    return {
        "total":   total,
        "passed":  trials_passed,
        "failed":  trials_failed,
        "divergent": divergent,
    }


def print_convergence_report(results):
    """Print a pass/fail summary table for convergence trials."""
    print()
    print("=" * 60)
    print("  Convergence Trial Results")
    print("=" * 60)
    print(f"  Trials : {results['total']}")
    print(f"  Passed : {results['passed']}")
    print(f"  Failed : {results['failed']}")
    pct = 100.0 * results["passed"] / results["total"] if results["total"] else 0.0
    print(f"  Rate   : {pct:.1f}% PASS")
    print()

    if not results["divergent"]:
        print("  All trials converged. ✓")
    else:
        print(f"  {'Trial':>6}  {'Peers':>5}  {'Ops':>4}  {'Seed':>10}  Details")
        print("  " + "-" * 56)
        for d in results["divergent"]:
            print(
                f"  {d['trial']:>6}  {d['n_peers']:>5}  {d['ops']:>4}"
                f"  {d['seed']:>10}  {d['reason']}"
            )
            for diff in d["diffs"]:
                print(f"  {'':>28}  ↳ {diff}")
    print()


# ---------------------------------------------------------------------------
# Scalability analysis
# ---------------------------------------------------------------------------

_NPEER_RE = re.compile(r"^(\d+)peer$")


def _load_scalability_subdir(subdir):
    """Merge peer_*.log files; return (sends, max_applies) dicts.

    Uses the LATEST apply timestamp per op (= last peer to converge), which
    measures true system-wide convergence time rather than first-receiver latency.
    """
    all_sends = {}
    all_max_applies = {}
    try:
        entries = sorted(os.listdir(subdir))
    except OSError:
        return all_sends, all_max_applies

    for fname in entries:
        if not (fname.startswith("peer_") and fname.endswith(".log")):
            continue
        sends, applies = parse_log(os.path.join(subdir, fname))
        all_sends.update(sends)
        for key, ts in applies.items():
            if key not in all_max_applies or ts > all_max_applies[key]:
                all_max_applies[key] = ts

    return all_sends, all_max_applies


def _parse_cpu(subdir):
    """Return a list of CPU percentages parsed from cpu_N.txt files."""
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


def _compute_scalability_metrics(subdir, n_peers):
    """Compute convergence time, throughput, and CPU for one N-peer subdir."""
    sends, max_applies = _load_scalability_subdir(subdir)
    cpu_list = _parse_cpu(subdir)

    if not sends:
        return None

    matched_apply_ts = [max_applies[k] for k in sends if k in max_applies]
    dropped = len(sends) - len(matched_apply_ts)

    if not matched_apply_ts:
        return None

    first_send_us   = min(sends.values())
    last_apply_us   = max(matched_apply_ts)
    convergence_ms  = (last_apply_us - first_send_us) / 1000.0
    convergence_sec = convergence_ms / 1000.0

    total_sent = len(sends)
    delivered  = len(matched_apply_ts)
    drop_pct   = 100.0 * dropped / total_sent if total_sent > 0 else 0.0

    sys_throughput = delivered / convergence_sec if convergence_sec > 0 else 0.0
    per_peer_tput  = sys_throughput / n_peers if n_peers > 0 else 0.0
    avg_cpu        = sum(cpu_list) / len(cpu_list) if cpu_list else float("nan")

    return {
        "n_peers":        n_peers,
        "total_sent":     total_sent,
        "delivered":      delivered,
        "dropped":        dropped,
        "drop_pct":       drop_pct,
        "convergence_ms": convergence_ms,
        "sys_throughput": sys_throughput,
        "per_peer_tput":  per_peer_tput,
        "avg_cpu_pct":    avg_cpu,
        "n_cpu_samples":  len(cpu_list),
    }


def _detect_bottleneck(results):
    """Flag super-linear convergence growth (≥2× rate) or CPU ≥ 80%.

    Returns a list of (n_peers, reason) tuples.
    """
    flags = []
    prev = None
    for r in results:
        if prev is not None and prev["convergence_ms"] > 0:
            ratio_peers = r["n_peers"] / prev["n_peers"]
            ratio_conv  = r["convergence_ms"] / prev["convergence_ms"]
            if ratio_conv >= 2 * ratio_peers:
                flags.append((
                    r["n_peers"],
                    f"convergence time grew {ratio_conv:.1f}× "
                    f"while peers grew {ratio_peers:.1f}×",
                ))
        cpu = r["avg_cpu_pct"]
        if not math.isnan(cpu) and cpu >= 80:
            flags.append((r["n_peers"], f"avg CPU {cpu:.0f}% ≥ 80%"))
        prev = r
    return flags


def _fmt(v, decimals=1):
    if isinstance(v, float) and math.isnan(v):
        return "N/A"
    return f"{v:.{decimals}f}"


def analyze_scalability(scalability_dir):
    """Discover N-peer subdirs under scalability_dir and compute metrics.

    Returns a list of metric dicts sorted by peer count, or None if no
    usable data is found.
    """
    subdirs = []
    try:
        def _sort_key(name):
            mo = _NPEER_RE.match(name)
            return int(mo.group(1)) if mo else 0

        for entry in sorted(os.listdir(scalability_dir), key=_sort_key):
            mo = _NPEER_RE.match(entry)
            if not mo:
                continue
            n = int(mo.group(1))
            subdirs.append((n, os.path.join(scalability_dir, entry)))
    except OSError:
        return None

    if not subdirs:
        return None

    results = []
    for n, subdir in subdirs:
        r = _compute_scalability_metrics(subdir, n)
        if r is None:
            print(f"Warning: no LATENCY data in {subdir} — skipping")
            continue
        results.append(r)

    return results if results else None


def print_scalability_table(results):
    """Print the scalability summary table and bottleneck analysis."""
    print()
    print("=" * 82)
    print("  P2P Editor — Scalability Results")
    print("=" * 82)
    hdr = (
        f"  {'Peers':>5}  {'Sent':>6}  {'Rcvd':>6}  {'Drop%':>6}"
        f"  {'Conv ms':>8}  {'Sys ops/s':>10}  {'Per-peer ops/s':>15}  {'CPU%':>6}"
    )
    print(hdr)
    print("  " + "-" * 78)
    for r in results:
        print(
            f"  {r['n_peers']:>5}  {r['total_sent']:>6}  {r['delivered']:>6}"
            f"  {_fmt(r['drop_pct']):>6}"
            f"  {_fmt(r['convergence_ms']):>8}"
            f"  {_fmt(r['sys_throughput']):>10}"
            f"  {_fmt(r['per_peer_tput']):>15}"
            f"  {_fmt(r['avg_cpu_pct']):>6}"
        )
    print()
    print("  Columns:")
    print("    Conv ms       — wall time: first send → last apply across all peers")
    print("    Sys ops/s     — total unique ops converged per second")
    print("    Per-peer ops/s— sys_throughput / n_peers")
    print("    CPU%          — avg CPU per peer (/usr/bin/time); N/A if not collected")
    print()

    flags = _detect_bottleneck(results)
    print("  Bottleneck analysis:")
    if not flags:
        print("  No bottleneck detected.")
    else:
        for n, reason in flags:
            print(f"    {n} peers : {reason}")
    print()


def write_scalability_chart(results, path):
    """Generate a 3-panel line chart (convergence time, throughput, CPU)."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print(
            "matplotlib not available — skipping scalability chart "
            "(install with: pip install matplotlib)"
        )
        _print_ascii_chart(results)
        return

    peers = [r["n_peers"]        for r in results]
    conv  = [r["convergence_ms"] for r in results]
    tput  = [r["per_peer_tput"]  for r in results]
    cpu_raw = [r["avg_cpu_pct"]  for r in results]
    cpu   = [v if not math.isnan(v) else None for v in cpu_raw]

    fig, axes = plt.subplots(1, 3, figsize=(13, 4))
    fig.suptitle("P2P Editor — Scalability (100 ops/peer @ 5 ops/sec)", fontsize=13)

    ax = axes[0]
    ax.plot(peers, conv, marker="o", color="steelblue", linewidth=2)
    ax.set_title("Convergence Time")
    ax.set_xlabel("Peers")
    ax.set_ylabel("ms")
    ax.set_xticks(peers)
    ax.grid(True, linestyle="--", alpha=0.5)

    ax = axes[1]
    ax.plot(peers, tput, marker="s", color="darkorange", linewidth=2)
    ax.set_title("Per-Peer Throughput")
    ax.set_xlabel("Peers")
    ax.set_ylabel("ops/sec delivered")
    ax.set_xticks(peers)
    ax.grid(True, linestyle="--", alpha=0.5)

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
    print(f"Scalability chart saved to: {path}")


def _print_ascii_chart(results):
    """ASCII bar chart fallback when matplotlib is unavailable."""
    print()
    print("  Convergence time (ms) — ASCII chart")
    print("  " + "-" * 42)
    max_conv = max(r["convergence_ms"] for r in results) or 1
    for r in results:
        bar = "█" * int(r["convergence_ms"] / max_conv * 30)
        print(f"  {r['n_peers']:>3} peers │{bar:<30}│ {r['convergence_ms']:.0f} ms")
    print()
    print("  Per-peer throughput (ops/sec)")
    print("  " + "-" * 42)
    max_tput = max(r["per_peer_tput"] for r in results) or 1
    for r in results:
        bar = "█" * int(r["per_peer_tput"] / max_tput * 30)
        print(f"  {r['n_peers']:>3} peers │{bar:<30}│ {r['per_peer_tput']:.1f} ops/s")
    print()


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def fmt_ms(v):
    return f"{v:.1f}" if v == v else "N/A"  # NaN check


def print_summary_table(results):
    print()
    print("=" * 106)
    print("  P2P Editor Evaluation — Summary")
    print("=" * 106)
    hdr = (
        f"  {'Config':<22} {'Peers':>5} {'Sent':>6} {'Rcvd':>6}"
        f" {'Min µs':>10} {'Avg µs':>10} {'P50 µs':>10} {'P95 µs':>10} {'P99 µs':>10}"
        f" {'Max µs':>10} {'Drop%':>7} {'Conv':>6}"
    )
    print(hdr)
    print("  " + "-" * 102)
    for r in results:
        print(
            f"  {r['label']:<22} {r['n_peers']:>5} {r['total']:>6} {r['delivered']:>6}"
            f" {fmt_ms(r['min']):>10} {fmt_ms(r['avg']):>10} {fmt_ms(r['p50']):>10}"
            f" {fmt_ms(r['p95']):>10} {fmt_ms(r['p99']):>10} {fmt_ms(r['max']):>10}"
            f" {r['drop_pct']:>6.1f}% {r['converged']:>6}"
        )
        if r["drop_pct"] > 1.0:
            print(
                f"  *** WARNING: drop rate {r['drop_pct']:.1f}% > 1% — "
                "results may not be representative"
            )
        if r["negative"] > 0:
            print(
                f"  Note: {r['negative']} sample(s) excluded (negative delta — NTP skew)"
            )
        if r["converged"] == "false":
            print("  *** CONVERGENCE FAILURE: dump files differ across peers!")
    print()


def write_csv(results, conv_results, scale_results, csv_path):
    fieldnames = [
        "config", "n_peers", "total_ops", "delivered",
        "drop_pct", "min_us", "avg_us", "p50_us", "p95_us", "p99_us", "max_us",
        "converged", "trials", "trials_passed", "trials_failed",
        "convergence_ms", "sys_throughput_ops_sec",
        "per_peer_throughput_ops_sec", "avg_cpu_pct",
    ]
    with open(csv_path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()

        # Latency / convergence / concurrent rows
        for r in results:
            writer.writerow({
                "config":         r["label"],
                "n_peers":        r["n_peers"],
                "total_ops":      r["total"],
                "delivered":      r["delivered"],
                "drop_pct":       f"{r['drop_pct']:.2f}",
                "min_us":         fmt_ms(r["min"]),
                "avg_us":         fmt_ms(r["avg"]),
                "p50_us":         fmt_ms(r["p50"]),
                "p95_us":         fmt_ms(r["p95"]),
                "p99_us":         fmt_ms(r["p99"]),
                "max_us":         fmt_ms(r["max"]),
                "converged":      r["converged"],
                "trials":         "",
                "trials_passed":  "",
                "trials_failed":  "",
                "convergence_ms":               "",
                "sys_throughput_ops_sec":       "",
                "per_peer_throughput_ops_sec":  "",
                "avg_cpu_pct":                  "",
            })

        # Convergence trial summary row
        if conv_results:
            writer.writerow({
                "config":         "convergence",
                "n_peers":        "3-5",
                "total_ops":      "",
                "delivered":      "",
                "drop_pct":       "",
                "min_us":         "",
                "avg_us":         "",
                "p50_us":         "",
                "p95_us":         "",
                "p99_us":         "",
                "max_us":         "",
                "converged":      "true" if conv_results["failed"] == 0 else "false",
                "trials":         conv_results["total"],
                "trials_passed":  conv_results["passed"],
                "trials_failed":  conv_results["failed"],
                "convergence_ms":               "",
                "sys_throughput_ops_sec":       "",
                "per_peer_throughput_ops_sec":  "",
                "avg_cpu_pct":                  "",
            })

        # Scalability rows (one per N-peer configuration)
        if scale_results:
            for r in scale_results:
                writer.writerow({
                    "config":        f"scalability_{r['n_peers']}peer",
                    "n_peers":       r["n_peers"],
                    "total_ops":     r["total_sent"],
                    "delivered":     r["delivered"],
                    "drop_pct":      f"{r['drop_pct']:.2f}",
                    "min_us":        "",
                    "avg_us":        "",
                    "p50_us":        "",
                    "p95_us":        "",
                    "p99_us":        "",
                    "max_us":        "",
                    "converged":     "",
                    "trials":        "",
                    "trials_passed": "",
                    "trials_failed": "",
                    "convergence_ms":              f"{r['convergence_ms']:.1f}",
                    "sys_throughput_ops_sec":      f"{r['sys_throughput']:.1f}",
                    "per_peer_throughput_ops_sec": f"{r['per_peer_tput']:.1f}",
                    "avg_cpu_pct":                 _fmt(r["avg_cpu_pct"]),
                })

    print(f"CSV written to: {csv_path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

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
            csv_out = sys.argv[i + 1]
            i += 2
        elif sys.argv[i] == "--chart" and i + 1 < len(sys.argv):
            chart_out = sys.argv[i + 1]
            i += 2
        else:
            print(f"ERROR: unknown argument '{sys.argv[i]}'", file=sys.stderr)
            sys.exit(1)

    if not os.path.isdir(results_dir):
        print(f"ERROR: results directory not found: {results_dir}", file=sys.stderr)
        sys.exit(1)

    try:
        entries = sorted(os.listdir(results_dir))
    except OSError as exc:
        print(f"ERROR: cannot read results directory: {exc}", file=sys.stderr)
        sys.exit(1)

    results       = []
    conv_results  = None
    scale_results = None

    for entry in entries:
        if entry.startswith("."):
            continue
        subdir_path = os.path.join(results_dir, entry)
        if not os.path.isdir(subdir_path):
            continue

        if entry == "convergence":
            conv_results = analyze_convergence_trials(subdir_path)
            continue

        if entry == "scalability":
            scale_results = analyze_scalability(subdir_path)
            continue

        r = analyze_subdir(subdir_path, entry)
        if r["total"] == 0 and r["converged"] == "N/A":
            continue  # empty subdir — skip silently
        if r["total"] == 0:
            print(f"Warning: no LATENCY_SEND records found in {subdir_path}")
        results.append(r)

    if not results and conv_results is None and scale_results is None:
        print(f"No experiment subdirectories found under '{results_dir}'")
        sys.exit(1)

    if results:
        print_summary_table(results)

    if conv_results is not None:
        print_convergence_report(conv_results)

    if scale_results is not None:
        print_scalability_table(scale_results)
        # Generate chart at a default path unless overridden via --chart.
        _chart_path = chart_out or os.path.join(
            results_dir, "scalability", "scalability_chart.png"
        )
        write_scalability_chart(scale_results, _chart_path)
    elif chart_out:
        print("Warning: --chart specified but no scalability/ directory found")

    if csv_out:
        write_csv(results, conv_results, scale_results, csv_out)


if __name__ == "__main__":
    main()
