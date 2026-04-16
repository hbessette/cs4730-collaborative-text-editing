#!/usr/bin/env python3
"""Convergence analysis for p2p-editor.

Reads results produced by `run_eval.sh --eval convergence` and reports
pass/fail counts across all trials.

Usage:
    python3 scripts/analyze_convergence.py <convergence-dir> [--csv FILE] [--chart FILE]

<convergence-dir> should contain trial_NNN/ subdirectories, each holding
peer_*_dump.txt files and an optional meta.txt (trial=N n_peers=N ops=N seed=N).

Output:
  - Pass/fail summary table (stdout)
  - Divergence detail for each failed trial (stdout)
  - CSV file (--csv)
  - Bar chart PNG showing pass/fail counts by peer count (--chart;
    requires matplotlib)
"""

import csv
import os
import re
import sys

_TRIAL_RE = re.compile(r"^trial_(\d+)$")
_META_RE  = re.compile(r"(\w+)=(\S+)")


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def _read_meta(trial_dir):
    meta = {}
    try:
        with open(os.path.join(trial_dir, "meta.txt"), encoding="utf-8") as fh:
            for m in _META_RE.finditer(fh.read()):
                meta[m.group(1)] = m.group(2)
    except OSError:
        pass
    return meta


def _read_dump(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            content = fh.read().strip()
        return content if content else None
    except OSError:
        return None


def analyze_trials(convergence_dir):
    """Analyse every trial_NNN/ subdir.

    Returns a dict:
      total, passed, failed,
      divergent: list of per-failure dicts,
      by_peers:  {n_peers: {"passed": int, "failed": int}}
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

    passed = 0
    failed = 0
    divergent = []
    by_peers = {}

    for tdir in trial_dirs:
        meta     = _read_meta(tdir)
        trial_num = int(meta.get("trial",  0))
        n_peers   = int(meta.get("n_peers", 0))
        seed      = meta.get("seed", "?")
        ops       = meta.get("ops",  "?")

        dumps = {}
        for fname in sorted(os.listdir(tdir)):
            if not fname.endswith("_dump.txt"):
                continue
            m = re.match(r"peer_(\d+)_dump\.txt$", fname)
            if not m:
                continue
            content = _read_dump(os.path.join(tdir, fname))
            if content is not None:
                dumps[int(m.group(1))] = content

        bucket = by_peers.setdefault(n_peers, {"passed": 0, "failed": 0})

        if len(dumps) < 2:
            failed += 1
            bucket["failed"] += 1
            divergent.append({
                "trial":   trial_num,
                "n_peers": n_peers,
                "seed":    seed,
                "ops":     ops,
                "reason":  f"only {len(dumps)} dump(s) (expected {n_peers})",
                "diffs":   [],
            })
            continue

        ref  = dumps[min(dumps)]
        diffs = [
            f"peer_{i} differs from peer_{min(dumps)}"
            for i, c in dumps.items()
            if c != ref and i != min(dumps)
        ]

        if not diffs:
            passed += 1
            bucket["passed"] += 1
        else:
            failed += 1
            bucket["failed"] += 1
            divergent.append({
                "trial":   trial_num,
                "n_peers": n_peers,
                "seed":    seed,
                "ops":     ops,
                "reason":  f"{len(diffs)} peer(s) diverged",
                "diffs":   diffs,
            })

    return {
        "total":    passed + failed,
        "passed":   passed,
        "failed":   failed,
        "divergent": divergent,
        "by_peers":  by_peers,
    }


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def print_report(results):
    print()
    print("=" * 62)
    print("  P2P Editor — Convergence Trial Results")
    print("=" * 62)
    print(f"  Trials : {results['total']}")
    print(f"  Passed : {results['passed']}")
    print(f"  Failed : {results['failed']}")
    pct = 100.0 * results["passed"] / results["total"] if results["total"] else 0.0
    print(f"  Rate   : {pct:.1f}% PASS")

    # Per-peer-count breakdown
    if results["by_peers"]:
        print()
        print(f"  {'Peers':>5}  {'Passed':>7}  {'Failed':>7}  {'Rate':>7}")
        print("  " + "-" * 32)
        for n in sorted(results["by_peers"]):
            b = results["by_peers"][n]
            t = b["passed"] + b["failed"]
            r = 100.0 * b["passed"] / t if t else 0.0
            print(f"  {n:>5}  {b['passed']:>7}  {b['failed']:>7}  {r:>6.1f}%")
    print()

    if not results["divergent"]:
        print("  All trials converged. ✓")
    else:
        print(f"  {'Trial':>6}  {'Peers':>5}  {'Ops':>4}  {'Seed':>10}  Details")
        print("  " + "-" * 58)
        for d in results["divergent"]:
            print(
                f"  {d['trial']:>6}  {d['n_peers']:>5}  {d['ops']:>4}"
                f"  {d['seed']:>10}  {d['reason']}"
            )
            for diff in d["diffs"]:
                print(f"  {'':>28}  ↳ {diff}")
    print()


def write_csv(results, path):
    # Summary row + per-peer-count rows
    fields = ["config", "n_peers", "trials", "passed", "failed", "pass_rate_pct"]
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=fields)
        w.writeheader()
        total = results["total"]
        pct   = 100.0 * results["passed"] / total if total else 0.0
        w.writerow({
            "config":        "convergence_all",
            "n_peers":       "3-5",
            "trials":        total,
            "passed":        results["passed"],
            "failed":        results["failed"],
            "pass_rate_pct": f"{pct:.1f}",
        })
        for n in sorted(results["by_peers"]):
            b = results["by_peers"][n]
            t = b["passed"] + b["failed"]
            r = 100.0 * b["passed"] / t if t else 0.0
            w.writerow({
                "config":        f"convergence_{n}peer",
                "n_peers":       n,
                "trials":        t,
                "passed":        b["passed"],
                "failed":        b["failed"],
                "pass_rate_pct": f"{r:.1f}",
            })
    print(f"CSV written to: {path}")


def write_chart(results, path):
    """Stacked bar chart: passed vs failed, grouped by peer count."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print(
            "matplotlib not available — skipping convergence chart "
            "(install with: pip install matplotlib)"
        )
        _print_ascii_chart(results)
        return

    by_peers = results["by_peers"]
    if not by_peers:
        print("No per-peer data to chart.")
        return

    peer_counts = sorted(by_peers)
    passed_vals = [by_peers[n]["passed"] for n in peer_counts]
    failed_vals = [by_peers[n]["failed"] for n in peer_counts]
    x = np.arange(len(peer_counts))

    fig, ax = plt.subplots(figsize=(max(5, len(peer_counts) * 1.5 + 2), 5))
    bars_pass = ax.bar(x, passed_vals, label="Passed", color="steelblue")
    bars_fail = ax.bar(x, failed_vals, bottom=passed_vals, label="Failed", color="firebrick")

    # Annotate pass rate above each bar
    for i, (p, f) in enumerate(zip(passed_vals, failed_vals)):
        t = p + f
        rate = 100.0 * p / t if t else 0.0
        ax.text(i, t + 0.3, f"{rate:.0f}%", ha="center", va="bottom", fontsize=9)

    ax.set_title("P2P Editor — Convergence Trials by Peer Count")
    ax.set_xlabel("Peers per trial")
    ax.set_ylabel("Trial count")
    ax.set_xticks(x)
    ax.set_xticklabels([str(n) for n in peer_counts])
    ax.legend()
    ax.grid(axis="y", linestyle="--", alpha=0.5)

    fig.tight_layout()
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"Convergence chart saved to: {path}")


def _print_ascii_chart(results):
    print()
    print("  Pass rate by peer count — ASCII chart")
    print("  " + "-" * 44)
    for n in sorted(results["by_peers"]):
        b = results["by_peers"][n]
        t = b["passed"] + b["failed"]
        r = b["passed"] / t if t else 0.0
        bar = "█" * int(r * 30)
        print(f"  {n} peers │{bar:<30}│ {r * 100:.0f}%")
    print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        sys.exit(0 if len(sys.argv) > 1 else 1)

    convergence_dir = sys.argv[1]
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

    if not os.path.isdir(convergence_dir):
        print(f"ERROR: directory not found: {convergence_dir}", file=sys.stderr)
        sys.exit(1)

    results = analyze_trials(convergence_dir)
    if results is None:
        print(f"No trial_NNN/ subdirectories found under '{convergence_dir}'")
        sys.exit(1)

    print_report(results)

    if csv_out:
        write_csv(results, csv_out)

    chart_path = chart_out or os.path.join(
        os.path.dirname(convergence_dir.rstrip("/")), "convergence_chart.png"
    )
    write_chart(results, chart_path)


if __name__ == "__main__":
    main()
