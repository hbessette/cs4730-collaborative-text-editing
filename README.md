# p2p Collaborative Text Editor

A peer-to-peer collaborative text editor built in C++.

## Dependencies

- CMake 3.16+
- A C++11-capable compiler (GCC, Clang)
- pthreads (standard on Linux/macOS)
- [FTXUI](https://github.com/ArthurSonzogni/FTXUI) — fetched automatically via CMake FetchContent

## Build

```bash
cmake -B build
cmake --build build
```

## Run

### Starting the first peer

The first peer starts the document. Use `--first` to skip the initial state sync:

```bash
./build/p2p-editor --first
```

You can optionally specify a port (default is 10000):

```bash
./build/p2p-editor --first --port 10000
```

### Joining as a subsequent peer

Additional peers connect by providing the address of any existing peer via `--peer`:

```bash
./build/p2p-editor --peer 127.0.0.1:10000
```

Use `--port` if you need a non-default port (required when running multiple peers on the same machine):

```bash
./build/p2p-editor --port 10010 --peer 127.0.0.1:10000
```

Multiple `--peer` flags can be provided:

```bash
./build/p2p-editor --port 10020 --peer 127.0.0.1:10000 --peer 127.0.0.1:10010
```

### Port layout

Each instance occupies four consecutive ports starting from `--port P`:

| Port  | Purpose              |
|-------|----------------------|
| P     | CRDT operations (UDP)|
| P+1   | Heartbeat / peer discovery (UDP) |
| P+2   | State sync (TCP)     |
| P+3   | Cursor sync (UDP)    |

> **Cluster note:** The cluster only permits ports ≥ 10000. The default port (10000) and the `latency_eval.sh` script are configured accordingly. When running multiple peers on the same node, use a 10-port gap (e.g. 10000, 10010, 10020) to avoid collisions across the four-port range.

### Keyboard shortcuts

| Key                     | Action                          |
|-------------------------|---------------------------------|
| Printable chars / Enter | Insert at cursor                |
| Backspace / Delete      | Delete character                |
| Arrow keys              | Move cursor                     |
| Home / End              | Jump to line start / end        |
| Escape                  | Quit (broadcasts LEAVE to peers)|

### Logging

By default, logs are written to `logs/<siteID>.log` in the current directory (the `logs/` folder is created automatically). Override the path with `--log-path FILE`:

```bash
./build/p2p-editor --first --port 10000
# → writes to logs/<siteHex>.log

./build/p2p-editor --port 10010 --peer 127.0.0.1:10000 --log-path /tmp/peer-B.log
# → writes to /tmp/peer-B.log
```

Log lines follow the format:

```
[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] [SITEHHEX] [module] message
```

Logged events include peer join/leave, state sync start/result, and (when debug logging is compiled in) every operation sent and received with a millisecond timestamp for latency measurement.

To correlate send and receive latency across peers, join `OP_SEND` entries from one peer's log with `OP_RECV` entries from another on the same `clock` + `siteID` values.

#### Debug logging

Operation-level `DEBUG` entries are compiled out by default. Enable them at build time:

```bash
cmake -B build -DENABLE_DEBUG_LOG=ON
cmake --build build -j4
```

To build without debug logging (production):

```bash
cmake -B build -DENABLE_DEBUG_LOG=OFF
cmake --build build -j4
```

## Headless mode

The editor can run without a terminal UI, driven by a script file. This is used for automated testing and evaluation.

```bash
./build/p2p-editor --first --headless --script path/to/script.txt
```

Script commands (one per line; `#` and blank lines are ignored):

| Command            | Effect                                    |
|--------------------|-------------------------------------------|
| `INSERT <pos> <c>` | Insert character `c` at visible position `pos` |
| `DELETE <pos>`     | Delete character at visible position `pos` |
| `SLEEP <ms>`       | Sleep for `ms` milliseconds               |
| `DUMP`             | Print the current document to stdout      |
| `QUIT`             | Exit (EOF also exits)                     |

## Latency evaluation

End-to-end operation propagation latency can be measured across a cluster using the scripts in `scripts/`.

### Requirements

- Passwordless SSH from the coordinating machine to each cluster node
- The `p2p-editor` binary built on each cluster node
- NTP-synchronized clocks on all nodes
- Python 3 on the coordinating machine

### SSH setup

The eval script connects to cluster nodes over SSH without prompting for a password. If your cluster nodes share a home directory, this one-time setup is all that's needed.

**1. Create the `.ssh` directory if it doesn't exist:**

```bash
mkdir -p ~/.ssh
chmod 700 ~/.ssh
```

**2. Generate a key (skip if you already have `~/.ssh/id_ed25519`):**

```bash
ssh-keygen -t ed25519
# Leave the passphrase blank (just press Enter twice)
```

If you accidentally set a passphrase and want to remove it:

```bash
ssh-keygen -p -f ~/.ssh/id_ed25519
# Enter your current passphrase, then press Enter twice for the new one
```

**3. Authorize the key:**

```bash
cat ~/.ssh/id_ed25519.pub >> ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys
```

Because the home directory is shared across nodes, this immediately allows passwordless SSH to every node.

**4. Test:**

```bash
ssh node01   # should connect without a password prompt
```

### Setup

Edit `scripts/cluster.conf` with the hostnames of your cluster nodes (one per line). The first host is the **sender**; the rest are **receivers**.

```
node01   # sender — generates 1000 INSERT operations
node02   # receiver
node03   # receiver
node04   # receiver
node05   # receiver
```

### Run

```bash
# Run 2-peer and 5-peer experiments (reads hostnames from cluster.conf)
./scripts/latency_eval.sh --binary ./build/p2p-editor --config scripts/cluster.conf
```

Or pass hostnames directly:

```bash
./scripts/latency_eval.sh --binary ./build/p2p-editor node01 node02 node03 node04 node05
```

`--binary` is the path to `p2p-editor` **on the remote nodes**. It can also be set via the `$REMOTE_BINARY` environment variable.

The script:
1. Copies the sender/receiver scripts to each node
2. Launches receivers, waits 5 s for them to stabilize
3. Runs the sender (1000 INSERT ops over ~12 s)
4. Collects all log files into `logs/2peer/` and `logs/5peer/`

### Analyze

```bash
python3 scripts/analyze_latency.py logs/
```

This parses the `LATENCY_SEND` / `LATENCY_APPLY` records from each log, joins them on the unique `(siteID, clock)` operation key, and reports:

- P50 / P95 / P99 end-to-end latency per configuration
- Per-configuration drop rate (ops sent but never received)
- Histogram PNG saved to `scripts/latency_histogram.png`
- A formatted section suitable for pasting into an evaluation report

### Generate a custom sender script

```bash
# 1000 ops, 10 ms sleep every 10 inserts (default)
python3 scripts/gen_inserts.py > my_script.txt

# 2000 ops, 5 ms sleep every 5 inserts
python3 scripts/gen_inserts.py --count 2000 --sleep-ms 5 --batch 5 > my_script.txt
```

## Test

```bash
./build/tests
```

## Project Structure

```
.
├── CMakeLists.txt
├── include/       # Header files
├── src/           # Application source files
├── tests/         # Test source files
├── scripts/       # Utility scripts
└── docs/          # Documentation
```
