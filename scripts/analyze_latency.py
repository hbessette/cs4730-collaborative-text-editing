#!/usr/bin/env python3
"""Analyze latency logs from a p2p-editor experiment.

Usage:
    python3 eval/analyze_latency.py <results_dir>

<results_dir> must contain subdirectories named "2peer" and/or "5peer", each
holding log files produced by latency_eval.sh:
    sender.log          -- LATENCY_SEND records from the sending node
    receiver_0.log      -- LATENCY_APPLY records from receiver 0
    receiver_1.log ...  -- (additional receivers for 5-peer)

Log line format written by the instrumented pipeline:
    [YYYY-MM-DD HH:MM:SS.mmm] [INFO ] [SITEHHEX] [latency] LATENCY_SEND siteID=X clock=Y ts_us=Z
    [YYYY-MM-DD HH:MM:SS.mmm] [INFO ] [SITEHHEX] [latency] LATENCY_APPLY siteID=X clock=Y ts_us=Z

Cross-machine latency is computed as:
    latency_us = apply_ts_us - send_ts_us

Timestamps are in microseconds (us) for sub-millisecond resolution.
This requires NTP-synchronized clocks on all cluster nodes. On a typical
university LAN, NTP drift is < 5 ms, well within the 200 ms target.

For each configuration the script reports:
    - Sample count, drop rate
    - P50 / P95 / P99 latency in milliseconds
    - ASCII histogram
    - A formatted section ready to paste into an evaluation report
"""

import os
import re
import sys

# ---------------------------------------------------------------------------
# Log parsing
# ---------------------------------------------------------------------------

# Matches a log line produced by Logger::log():
#   [timestamp] [LEVEL ] [SITEHHEX] [module] message
_LOG_RE = re.compile(
    r"^\[(?P<ts>[^\]]+)\]\s+\[(?P<level>[^\]]+)\]\s+\[(?P<site>[^\]]+)\]\s+"
    r"\[(?P<module>[^\]]+)\]\s+(?P<msg>.*)$"
)
_SEND_RE  = re.compile(r"LATENCY_SEND\s+siteID=(\d+)\s+clock=(\d+)\s+ts_us=(\d+)")
_APPLY_RE = re.compile(r"LATENCY_APPLY\s+siteID=(\d+)\s+clock=(\d+)\s+ts_us=(\d+)")


def parse_log(path):
    """Return (sends, applies) dicts keyed by (siteID, clock) -> ts_ms (int)."""
    sends = {}
    applies = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = _LOG_RE.match(line.rstrip())
                if not m or m.group("module") != "latency":
                    continue
                msg = m.group("msg")
                sm = _SEND_RE.search(msg)
                if sm:
                    key = (int(sm.group(1)), int(sm.group(2)))
                    sends[key] = int(sm.group(3))
                    continue
                am = _APPLY_RE.search(msg)
                if am:
                    key = (int(am.group(1)), int(am.group(2)))
                    applies[key] = int(am.group(3))
    except FileNotFoundError:
        pass
    return sends, applies


def load_config(config_dir):
    """Load all log files under config_dir; return merged sends and applies."""
    all_sends = {}
    all_applies = {}  # key -> earliest apply ts across all receiver logs

    for fname in sorted(os.listdir(config_dir)):
        if not fname.endswith(".log"):
            continue
        sends, applies = parse_log(os.path.join(config_dir, fname))
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
    # nearest-rank method
    idx = max(0, int(len(s) * p / 100.0) - 1)
    return s[idx]


def analyze_config(config_dir, label):
    sends, applies = load_config(config_dir)
    latencies = []
    negative = 0
    dropped = 0

    for key, send_ts in sends.items():
        if key in applies:
            delta_us = applies[key] - send_ts
            if delta_us >= 0:
                latencies.append(delta_us / 1000.0)  # store as ms (float)
            else:
                negative += 1   # NTP skew artifact — exclude but count
        else:
            dropped += 1

    total = len(sends)
    return {
        "label":     label,
        "total":     total,
        "delivered": len(latencies),
        "dropped":   dropped,
        "negative":  negative,
        "drop_pct":  dropped / total * 100 if total > 0 else 0.0,
        "p50":       percentile(latencies, 50),
        "p95":       percentile(latencies, 95),
        "p99":       percentile(latencies, 99),
        "latencies": latencies,
    }


# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------

def print_summary_table(results):
    print()
    print("=" * 66)
    print("  Latency Evaluation — Summary")
    print("=" * 66)
    hdr = f"  {'Config':<10} {'Sent':>6} {'Rcvd':>6} {'P50 ms':>8} {'P95 ms':>8} {'P99 ms':>8} {'Drop%':>7}"
    print(hdr)
    print("  " + "-" * 62)
    for r in results:
        print(
            f"  {r['label']:<10} {r['total']:>6} {r['delivered']:>6}"
            f" {r['p50']:>8.1f} {r['p95']:>8.1f} {r['p99']:>8.1f}"
            f" {r['drop_pct']:>6.1f}%"
        )
        if r["drop_pct"] > 1.0:
            print(
                f"  *** WARNING: drop rate {r['drop_pct']:.1f}% > 1% — "
                "results may not be representative"
            )
        if r["negative"] > 0:
            print(
                f"  Note: {r['negative']} samples excluded (negative delta — NTP skew)"
            )
    print()


def print_histogram(latencies, label, n_buckets=20, bar_width=40):
    if not latencies:
        print(f"  [{label}] no samples\n")
        return
    lo = min(latencies)
    hi = max(latencies)
    if lo == hi:
        print(f"  [{label}] all {len(latencies)} samples = {lo} ms\n")
        return
    bucket_w = (hi - lo) / n_buckets
    counts = [0] * n_buckets
    for v in latencies:
        idx = min(int((v - lo) / bucket_w), n_buckets - 1)
        counts[idx] += 1
    max_count = max(counts) or 1
    scale = bar_width / max_count

    print(f"  Latency histogram — {label}  "
          f"(n={len(latencies)}, range {lo}–{hi} ms, bucket ≈{bucket_w:.1f} ms)")
    for i, c in enumerate(counts):
        lo_b = lo + i * bucket_w
        bar = "#" * int(c * scale)
        print(f"  {lo_b:7.1f} ms | {bar:<{bar_width}} {c}")
    print()


def print_report_section(results):
    print("=" * 66)
    print("  Evaluation Report Section")
    print("=" * 66)
    print()
    print("### End-to-End Operation Propagation Latency")
    print()
    print("We measured end-to-end latency as the wall-clock time between")
    print("an operation being serialized and transmitted (LATENCY_SEND) and")
    print("the moment it was applied to the remote CRDT (LATENCY_APPLY).")
    print("Timestamps were captured using the system clock (UTC epoch µs);")
    print("cluster nodes are NTP-synchronized.")
    print()
    for r in results:
        n = r["total"]
        d = r["delivered"]
        dp = r["drop_pct"]
        print(f"**{r['label']} configuration** "
              f"({n} operations sent, {d} delivered, {dp:.1f}% drop rate):")
        print(f"  | Percentile | Latency |")
        print(f"  |------------|---------|")
        print(f"  | P50        | {r['p50']:>6.1f} ms |")
        print(f"  | P95        | {r['p95']:>6.1f} ms |")
        print(f"  | P99        | {r['p99']:>6.1f} ms |")
        target_met = "YES" if r["p95"] < 200 else "NO"
        print(f"  Target (P95 < 200 ms): {target_met}")
        print()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    results_dir = sys.argv[1]
    configs = [("2peer", "2-peer"), ("5peer", "5-peer")]
    results = []

    for subdir, label in configs:
        path = os.path.join(results_dir, subdir)
        if os.path.isdir(path):
            r = analyze_config(path, label)
            if r["total"] == 0:
                print(f"Warning: no LATENCY_SEND records found in {path}")
            results.append(r)

    if not results:
        print(f"Error: no config directories (2peer, 5peer) found under {results_dir}")
        sys.exit(1)

    print_summary_table(results)

    for r in results:
        print_histogram(r["latencies"], r["label"])

    print_report_section(results)


if __name__ == "__main__":
    main()
