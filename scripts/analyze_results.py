#!/usr/bin/env python3
"""Comprehensive analysis of p2p-editor evaluation results.

Usage:
    python3 scripts/analyze_results.py <results-dir> [--csv FILE]

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

CSV columns: config,n_peers,total_ops,delivered,drop_pct,p50_ms,p95_ms,p99_ms,
             converged,trials,trials_passed,trials_failed
"""

import csv
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

    for key, send_ts_us in sends.items():
        if key in applies:
            delta_us = applies[key] - send_ts_us
            if delta_us >= 0:
                latencies_ms.append(delta_us / 1000.0)
            else:
                negative += 1
        else:
            dropped += 1

    total = len(sends)
    drop_pct = dropped / total * 100 if total > 0 else 0.0

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
        "p50":        percentile(latencies_ms, 50),
        "p95":        percentile(latencies_ms, 95),
        "p99":        percentile(latencies_ms, 99),
        "converged":  converged,
        "latencies":  latencies_ms,
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
# Output
# ---------------------------------------------------------------------------

def fmt_ms(v):
    return f"{v:.1f}" if v == v else "N/A"  # NaN check


def print_summary_table(results):
    print()
    print("=" * 78)
    print("  P2P Editor Evaluation — Summary")
    print("=" * 78)
    hdr = (
        f"  {'Config':<22} {'Peers':>5} {'Sent':>6} {'Rcvd':>6}"
        f" {'P50 ms':>8} {'P95 ms':>8} {'P99 ms':>8} {'Drop%':>7} {'Conv':>6}"
    )
    print(hdr)
    print("  " + "-" * 74)
    for r in results:
        p50 = fmt_ms(r["p50"])
        p95 = fmt_ms(r["p95"])
        p99 = fmt_ms(r["p99"])
        print(
            f"  {r['label']:<22} {r['n_peers']:>5} {r['total']:>6} {r['delivered']:>6}"
            f" {p50:>8} {p95:>8} {p99:>8} {r['drop_pct']:>6.1f}% {r['converged']:>6}"
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


def write_csv(results, conv_results, csv_path):
    fieldnames = [
        "config", "n_peers", "total_ops", "delivered",
        "drop_pct", "p50_ms", "p95_ms", "p99_ms", "converged",
        "trials", "trials_passed", "trials_failed",
    ]
    with open(csv_path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for r in results:
            writer.writerow({
                "config":         r["label"],
                "n_peers":        r["n_peers"],
                "total_ops":      r["total"],
                "delivered":      r["delivered"],
                "drop_pct":       f"{r['drop_pct']:.2f}",
                "p50_ms":         fmt_ms(r["p50"]),
                "p95_ms":         fmt_ms(r["p95"]),
                "p99_ms":         fmt_ms(r["p99"]),
                "converged":      r["converged"],
                "trials":         "",
                "trials_passed":  "",
                "trials_failed":  "",
            })
        if conv_results:
            writer.writerow({
                "config":         "convergence",
                "n_peers":        "3-5",
                "total_ops":      "",
                "delivered":      "",
                "drop_pct":       "",
                "p50_ms":         "",
                "p95_ms":         "",
                "p99_ms":         "",
                "converged":      "true" if conv_results["failed"] == 0 else "false",
                "trials":         conv_results["total"],
                "trials_passed":  conv_results["passed"],
                "trials_failed":  conv_results["failed"],
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
    csv_out = None

    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == "--csv" and i + 1 < len(sys.argv):
            csv_out = sys.argv[i + 1]
            i += 2
        else:
            print(f"ERROR: unknown argument '{sys.argv[i]}'", file=sys.stderr)
            sys.exit(1)

    if not os.path.isdir(results_dir):
        print(f"ERROR: results directory not found: {results_dir}", file=sys.stderr)
        sys.exit(1)

    # Discover subdirectories (skip hidden dirs like .tmp_scripts).
    try:
        entries = sorted(os.listdir(results_dir))
    except OSError as exc:
        print(f"ERROR: cannot read results directory: {exc}", file=sys.stderr)
        sys.exit(1)

    results = []
    conv_results = None

    for entry in entries:
        if entry.startswith("."):
            continue
        subdir_path = os.path.join(results_dir, entry)
        if not os.path.isdir(subdir_path):
            continue

        # Detect the convergence trial directory (contains trial_NNN/ subdirs).
        if entry == "convergence":
            conv_results = analyze_convergence_trials(subdir_path)
            continue

        r = analyze_subdir(subdir_path, entry)
        if r["total"] == 0 and r["converged"] == "N/A":
            continue  # empty subdir — skip silently
        if r["total"] == 0:
            print(f"Warning: no LATENCY_SEND records found in {subdir_path}")
        results.append(r)

    if not results and conv_results is None:
        print(f"No experiment subdirectories found under '{results_dir}'")
        sys.exit(1)

    if results:
        print_summary_table(results)

    if conv_results is not None:
        print_convergence_report(conv_results)
    elif not results:
        print(f"No experiment subdirectories found under '{results_dir}'")
        sys.exit(1)

    if csv_out:
        write_csv(results, conv_results, csv_out)


if __name__ == "__main__":
    main()
