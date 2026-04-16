#!/usr/bin/env python3
"""Generate a random INSERT/DELETE headless script for convergence testing.

Each peer in a trial runs a unique random script derived from its seed so
that every trial exercises different conflict patterns while remaining
fully reproducible.

Usage:
    python3 gen_convergence_script.py [OPTIONS] > script.txt

Options:
    --ops N           Total operations (default: 200)
    --insert-prob P   Probability of INSERT vs DELETE, 0.0–1.0 (default: 0.7)
    --peer-id N       Peer index; offsets character set (default: 0)
    --seed N          RNG seed for reproducibility (default: random)
    --start-delay MS  Initial SLEEP before any ops in ms (default: 0)
    --batch N         Ops per sleep interval (default: 10)
    --sleep-ms N      ms to sleep between batches (default: 10)
    --drain-ms MS     Sleep after last op before DUMP in ms (default: 5000)
"""

import argparse
import random
import string
import sys


def main():
    parser = argparse.ArgumentParser(
        description="Generate random INSERT/DELETE script for convergence testing."
    )
    parser.add_argument("--ops",          type=int,   default=200)
    parser.add_argument("--insert-prob",  type=float, default=0.7)
    parser.add_argument("--peer-id",      type=int,   default=0)
    parser.add_argument("--seed",         type=int,   default=None)
    parser.add_argument("--start-delay",  type=int,   default=0)
    parser.add_argument("--batch",        type=int,   default=10)
    parser.add_argument("--sleep-ms",     type=int,   default=10)
    parser.add_argument("--drain-ms",     type=int,   default=5000)
    args = parser.parse_args()

    if not (0.0 < args.insert_prob <= 1.0):
        print("ERROR: --insert-prob must be in (0, 1]", file=sys.stderr)
        sys.exit(1)

    rng = random.Random(args.seed)
    chars = string.ascii_lowercase
    char_offset = (args.peer_id * 7) % 26

    print(f"SLEEP {args.start_delay}")

    local_len = 0
    batch_count = 0
    for i in range(args.ops):
        if local_len > 0 and rng.random() > args.insert_prob:
            pos = rng.randint(0, local_len - 1)
            print(f"DELETE {pos}")
            local_len -= 1
        else:
            pos = rng.randint(0, local_len)
            ch = chars[(i + char_offset) % 26]
            print(f"INSERT {pos} {ch}")
            local_len += 1

        batch_count += 1
        if batch_count >= args.batch and i < args.ops - 1:
            print(f"SLEEP {args.sleep_ms}")
            batch_count = 0

    # Quiescence: wait for in-flight ops from all peers to arrive before dumping.
    print(f"SLEEP {args.drain_ms}")
    print("DUMP")
    print("QUIT")


if __name__ == "__main__":
    main()
