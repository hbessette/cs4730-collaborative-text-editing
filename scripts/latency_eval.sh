#!/usr/bin/env bash
# latency_eval.sh — Run end-to-end latency evaluation for p2p-editor.
#
# Usage:
#   ./scripts/latency_eval.sh [OPTIONS]
#
# Options:
#   --binary PATH    Path to p2p-editor binary (default: build/p2p-editor
#                    relative to repo root; also read from $REMOTE_BINARY)
#   --config FILE    Cluster config file (default: scripts/cluster.conf)
#   --results DIR    Directory to store logs/results (default: logs/)
#   --no-analyze     Skip running analyze_latency.py at the end
#   -h, --help       Show this help
#
# The cluster config must list one host per line (user@hostname or hostname).
# The FIRST host is the sender; remaining hosts are receivers.
# Passwordless SSH from this machine to every host is required.
# All hosts must share the same filesystem (NFS), so the binary and result
# paths are identical on every node — no scp is needed.
#
# Port layout per node (10-port gap between nodes):
#   data port P  →  hb=P+1, tcp-sync=P+2, cursor-sync=P+3
#   sender: 10000, receiver[0]: 10010, receiver[1]: 10020, …
# Note: the cluster requires ports >= 10000.
#
# Experiment configs run:
#   2-peer  — sender + 1 receiver  (always)
#   5-peer  — sender + 4 receivers (skipped if fewer than 5 hosts)

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults (override via env or CLI flags)
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

CLUSTER_CONF="${CLUSTER_CONF:-${SCRIPT_DIR}/cluster.conf}"
BINARY="${REMOTE_BINARY:-${REPO_DIR}/build/p2p-editor}"
RESULTS_DIR="${RESULTS_DIR:-${REPO_DIR}/logs}"
SENDER_SCRIPT="${SCRIPT_DIR}/latency_sender.txt"
RECEIVER_SCRIPT="${SCRIPT_DIR}/latency_receiver.txt"
RUN_ANALYZE=1

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary)      BINARY="$2";       shift 2 ;;
        --config)      CLUSTER_CONF="$2"; shift 2 ;;
        --results)     RESULTS_DIR="$2";  shift 2 ;;
        --no-analyze)  RUN_ANALYZE=0;     shift   ;;
        -h|--help)     usage; exit 0      ;;
        *) echo "ERROR: unknown argument '$1'" >&2; usage; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Validate inputs
# ---------------------------------------------------------------------------

if [[ ! -f "$BINARY" ]]; then
    echo "ERROR: binary not found at '${BINARY}'" >&2
    echo "       Build with: cmake --build build" >&2
    exit 1
fi
if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: binary '${BINARY}' is not executable" >&2
    exit 1
fi
if [[ ! -f "$SENDER_SCRIPT" ]]; then
    echo "ERROR: sender script not found at '${SENDER_SCRIPT}'" >&2
    exit 1
fi
if [[ ! -f "$RECEIVER_SCRIPT" ]]; then
    echo "ERROR: receiver script not found at '${RECEIVER_SCRIPT}'" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Parse cluster.conf
# ---------------------------------------------------------------------------

declare -a HOSTS=()
while IFS= read -r line; do
    # Strip inline comments and surrounding whitespace
    line="${line%%#*}"
    line="${line#"${line%%[![:space:]]*}"}"   # leading whitespace
    line="${line%"${line##*[![:space:]]}"}"   # trailing whitespace
    [[ -z "$line" ]] && continue
    HOSTS+=("$line")
done < "$CLUSTER_CONF"

N_HOSTS="${#HOSTS[@]}"
if [[ "$N_HOSTS" -lt 2 ]]; then
    echo "ERROR: need at least 2 hosts in '${CLUSTER_CONF}' (found ${N_HOSTS})" >&2
    exit 1
fi

SENDER_ENTRY="${HOSTS[0]}"
SENDER_HOSTNAME="${SENDER_ENTRY#*@}"  # strip optional user@ prefix

echo "Cluster: ${N_HOSTS} hosts loaded from ${CLUSTER_CONF}"
echo "Sender : ${SENDER_ENTRY}"
for (( i=1; i<N_HOSTS; i++ )); do
    echo "Recv[$(( i-1 ))]: ${HOSTS[$i]}"
done
echo "Binary : ${BINARY}"
echo "Results: ${RESULTS_DIR}"

# ---------------------------------------------------------------------------
# Background-process tracking + cleanup
# ---------------------------------------------------------------------------

declare -a _BGPIDS=()

_cleanup() {
    local pids_left="${#_BGPIDS[@]}"
    if [[ "$pids_left" -gt 0 ]]; then
        echo ""
        echo "Cleaning up ${pids_left} background SSH process(es)…"
        for pid in "${_BGPIDS[@]}"; do
            kill "$pid" 2>/dev/null || true
        done
    fi
}
trap _cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Helper: strip user@ prefix from a host entry
# ---------------------------------------------------------------------------
strip_user() { echo "${1#*@}"; }

# ---------------------------------------------------------------------------
# run_experiment LABEL NUM_PEERS BASE_PORT
#
# Starts NUM_PEERS-1 receivers on HOSTS[1..NUM_PEERS-1] then the sender on
# HOSTS[0], all via SSH.  Logs are written directly to shared-filesystem paths
# under RESULTS_DIR/LABEL/.
# ---------------------------------------------------------------------------
run_experiment() {
    local label="$1"
    local num_peers="$2"
    local base_port="$3"

    # Absolute path for local mkdir; relative path used in remote commands
    # (each host cds to REPO_DIR first, so "logs/…" resolves correctly).
    local out_dir="${REPO_DIR}/logs/${label}"
    local rel_out_dir="logs/${label}"
    mkdir -p "$out_dir"

    local sender_log="${rel_out_dir}/sender.log"
    local sender_port="${base_port}"
    local recv_idx=0

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  Experiment : ${label}  (${num_peers} peers, base port ${base_port})"
    echo "  Output dir : ${out_dir}"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    # ------------------------------------------------------------------
    # Build receiver list (entries + ports) for use below
    # ------------------------------------------------------------------
    declare -a recv_entries=()
    declare -a recv_ports=()
    for (( i=1; i<num_peers; i++ )); do
        recv_entries+=("${HOSTS[$i]}")
        recv_ports+=("$(( base_port + i * 10 ))")
    done

    # ------------------------------------------------------------------
    # Start sender first (background) — no --peer flags needed; receivers
    # will connect to it and the sender discovers them via heartbeat.
    # ------------------------------------------------------------------
    echo "  [send]  ${SENDER_ENTRY}  port=${sender_port}"

    ssh \
        -o StrictHostKeyChecking=no \
        -o BatchMode=yes \
        -o ConnectTimeout=10 \
        "$SENDER_ENTRY" \
        "cd '${REPO_DIR}' && '${BINARY}' \
            --port ${sender_port} \
            --first \
            --headless \
            --script '${SENDER_SCRIPT}' \
            --log-path '${sender_log}' >/dev/null" \
        </dev/null &
    _BGPIDS+=($!)

    # Give sender time to bind its sockets before receivers try to connect.
    echo "  Waiting 3 s for sender to bind…"
    sleep 3

    # ------------------------------------------------------------------
    # Start receivers (sender is now up, so --peer syncState will succeed)
    # ------------------------------------------------------------------
    for (( i=0; i<${#recv_entries[@]}; i++ )); do
        local recv_entry="${recv_entries[$i]}"
        local recv_port="${recv_ports[$i]}"
        local recv_log="${rel_out_dir}/receiver_${recv_idx}.log"

        echo "  [recv ${recv_idx}] ${recv_entry}  port=${recv_port}"

        ssh \
            -o StrictHostKeyChecking=no \
            -o BatchMode=yes \
            -o ConnectTimeout=10 \
            "$recv_entry" \
            "cd '${REPO_DIR}' && '${BINARY}' \
                --port ${recv_port} \
                --peer ${SENDER_HOSTNAME}:${sender_port} \
                --headless \
                --script '${RECEIVER_SCRIPT}' \
                --log-path '${recv_log}' >/dev/null" \
            </dev/null &
        _BGPIDS+=($!)

        recv_idx=$(( recv_idx + 1 ))
    done

    echo "  Waiting for all nodes to complete…"
    for pid in "${_BGPIDS[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    _BGPIDS=()

    echo "  ${label} experiment complete."
    echo "  Logs written to: ${out_dir}/"
    ls -lh "${out_dir}/"
}

# ---------------------------------------------------------------------------
# Run experiments
# ---------------------------------------------------------------------------

# --- 2-peer (sender + 1 receiver) ---
run_experiment "2peer" 2 10000

# Brief pause between experiments to let OS release ports (UDP, so fast).
sleep 2

# --- 5-peer (sender + 4 receivers) — skipped if cluster has fewer than 5 ---
if [[ "$N_HOSTS" -ge 5 ]]; then
    run_experiment "5peer" 5 10000
else
    echo ""
    echo "Skipping 5-peer experiment: need 5 hosts, only ${N_HOSTS} configured."
fi

# ---------------------------------------------------------------------------
# Analyze results
# ---------------------------------------------------------------------------

if [[ "$RUN_ANALYZE" -eq 1 ]]; then
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  Analyzing latency results"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    python3 "${SCRIPT_DIR}/analyze_latency.py" "${RESULTS_DIR}"
else
    echo ""
    echo "Skipping analysis (--no-analyze).  Run manually:"
    echo "  python3 ${SCRIPT_DIR}/analyze_latency.py ${RESULTS_DIR}"
fi
