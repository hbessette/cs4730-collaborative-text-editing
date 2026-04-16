#!/usr/bin/env python3
"""Generate a headless INSERT script for latency evaluation.

Writes a sequence of INSERT commands to stdout, suitable for use with
  ./p2p-editor --headless --script <file>

Usage:
  python3 gen_inserts.py [--count N] [--sleep-ms M] [--batch B]

Options:
  --count N     Total number of INSERT operations (default: 1000)
  --sleep-ms M  Milliseconds to sleep after each batch (default: 10)
  --batch B     INSERT ops per sleep interval (default: 10)

The script ends with a 2-second drain sleep, DUMP, and QUIT so the sender
stays alive long enough for in-flight packets to arrive at receivers before
the log analysis sees the final document state.
"""
import argparse
import itertools
import string


def main():
    parser = argparse.ArgumentParser(
        description="Generate headless INSERT script for latency evaluation."
    )
    parser.add_argument("--count",    type=int, default=1000,
                        help="Total INSERT operations (default: 1000)")
    parser.add_argument("--sleep-ms", type=int, default=10,
                        help="Sleep ms between batches (default: 10)")
    parser.add_argument("--batch",    type=int, default=10,
                        help="INSERTs per sleep interval (default: 10)")
    args = parser.parse_args()

    chars = itertools.cycle(string.ascii_lowercase)
    pos = 0
    emitted = 0

    while emitted < args.count:
        batch = min(args.batch, args.count - emitted)
        for _ in range(batch):
            c = next(chars)
            print(f"INSERT {pos} {c}")
            pos += 1
            emitted += 1
        print(f"SLEEP {args.sleep_ms}")

    # Allow in-flight UDP packets to arrive at receivers before quitting.
    print("SLEEP 2000")
    print("DUMP")
    print("QUIT")


if __name__ == "__main__":
    main()
