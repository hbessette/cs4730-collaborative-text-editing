#!/usr/bin/env bash
# run_eval.sh — Orchestrate p2p-editor evaluation experiments.
#
# Usage:
#   scripts/run_eval.sh [OPTIONS]
#
# Options:
#   --eval TYPE      latency|convergence|concurrent|scalability (default: latency)
#   --peers N        Cap cluster.conf to N hosts (default: use all)
#   --ops N          Operations per writer (default: 1000)
#   --base-port N    Data port for peer 0 (default: 10000)
#   --binary PATH    Path to p2p-editor binary (default: build/p2p-editor)
#   --config FILE    Cluster config file (default: scripts/cluster.conf)
#   --results DIR    Directory for output logs (default: logs/)
#   --no-analyze     Skip post-experiment analysis
#   -h|--help        Show this help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

EVAL_TYPE="latency"
PEERS_CAP=0
OPS=1000
BASE_PORT=10000
BINARY="${REPO_DIR}/build/p2p-editor"
CLUSTER_CONF="${SCRIPT_DIR}/cluster.conf"
RESULTS_DIR="${REPO_DIR}/logs"
RUN_ANALYZE=1
# Convergence trial options
TRIALS=25
MIN_PEERS=3
MAX_PEERS=5
SEED_BASE=42

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --eval)        EVAL_TYPE="$2";    shift 2 ;;
        --peers)       PEERS_CAP="$2";   shift 2 ;;
        --ops)         OPS="$2";          shift 2 ;;
        --base-port)   BASE_PORT="$2";    shift 2 ;;
        --binary)      BINARY="$2";       shift 2 ;;
        --config)      CLUSTER_CONF="$2"; shift 2 ;;
        --results)     RESULTS_DIR="$2";  shift 2 ;;
        --no-analyze)  RUN_ANALYZE=0;     shift   ;;
        --trials)      TRIALS="$2";       shift 2 ;;
        --min-peers)   MIN_PEERS="$2";    shift 2 ;;
        --max-peers)   MAX_PEERS="$2";    shift 2 ;;
        --seed)        SEED_BASE="$2";    shift 2 ;;
        -h|--help)     usage; exit 0      ;;
        *) echo "ERROR: unknown argument '$1'" >&2; usage; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Validate
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
if [[ ! -f "$CLUSTER_CONF" ]]; then
    echo "ERROR: cluster config not found at '${CLUSTER_CONF}'" >&2
    exit 1
fi

case "$EVAL_TYPE" in
    latency|convergence|concurrent|scalability) ;;
    *) echo "ERROR: unknown eval type '${EVAL_TYPE}'" >&2; exit 1 ;;
esac

# ---------------------------------------------------------------------------
# Parse cluster.conf
# ---------------------------------------------------------------------------

declare -a ALL_HOSTS=()
while IFS= read -r line; do
    line="${line%%#*}"
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [[ -z "$line" ]] && continue
    ALL_HOSTS+=("$line")
done < "$CLUSTER_CONF"

if [[ "${#ALL_HOSTS[@]}" -lt 2 ]]; then
    echo "ERROR: need at least 2 hosts in '${CLUSTER_CONF}'" >&2
    exit 1
fi

if [[ "$PEERS_CAP" -gt 0 && "$PEERS_CAP" -lt "${#ALL_HOSTS[@]}" ]]; then
    ALL_HOSTS=("${ALL_HOSTS[@]:0:${PEERS_CAP}}")
fi

# ---------------------------------------------------------------------------
# Background PID tracking + cleanup
# ---------------------------------------------------------------------------

declare -a _BGPIDS=()
TMP_SCRIPT_DIR="${RESULTS_DIR}/.tmp_scripts"

_cleanup() {
    if [[ "${#_BGPIDS[@]}" -gt 0 ]]; then
        echo "Cleaning up ${#_BGPIDS[@]} background process(es)..."
        for pid in "${_BGPIDS[@]}"; do
            kill "$pid" 2>/dev/null || true
        done
    fi
    rm -rf "${TMP_SCRIPT_DIR}" 2>/dev/null || true
}
trap _cleanup EXIT INT TERM

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

strip_user() { echo "${1#*@}"; }

host_of() { strip_user "$1"; }

# port_for INDEX BASE  — each peer gets a 10-port gap
port_for() { echo $(( $2 + $1 * 10 )); }

ssh_run() {
    # ssh_run HOST REMOTE_CMD — run in background, append PID to _BGPIDS
    local host="$1"
    local cmd="$2"
    ssh \
        -o StrictHostKeyChecking=no \
        -o BatchMode=yes \
        -o ConnectTimeout=10 \
        "$host" \
        "$cmd" \
        </dev/null &
    _BGPIDS+=($!)
}

# ---------------------------------------------------------------------------
# run_experiment LABEL HOSTS_ARRAY_NAME BASE_PORT
#
# Dispatches to the appropriate eval-type runner.
# HOSTS_ARRAY_NAME is the name of a bash array variable (nameref).
# ---------------------------------------------------------------------------
run_experiment() {
    local label="$1"
    local -n _hosts="$2"
    local base="$3"

    local out_dir="${RESULTS_DIR}/${label}"
    mkdir -p "$out_dir"
    mkdir -p "${TMP_SCRIPT_DIR}"

    local n="${#_hosts[@]}"

    echo ""
    echo "================================================================="
    echo "  Experiment : ${label}  (${n} peers, base port ${base})"
    echo "  Output dir : ${out_dir}"
    echo "================================================================="

    case "$EVAL_TYPE" in
        latency)    _run_latency    "$label" _hosts "$base" "$out_dir" ;;
        convergence) _run_convergence "$label" _hosts "$base" "$out_dir" ;;
        concurrent)  _run_concurrent  "$label" _hosts "$base" "$out_dir" ;;
    esac

    echo "  Waiting for all peers to finish..."
    for pid in "${_BGPIDS[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    _BGPIDS=()

    echo "  ${label} complete. Logs in: ${out_dir}/"
    ls -lh "${out_dir}/" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Latency experiment helpers
# ---------------------------------------------------------------------------

_run_latency() {
    local label="$1"
    local -n __lhosts="$2"
    local base="$3"
    local out_dir="$4"
    local n="${#__lhosts[@]}"

    local sender_host="${__lhosts[0]}"
    local sender_port
    sender_port=$(port_for 0 "$base")
    local sender_hostname
    sender_hostname=$(host_of "$sender_host")

    # Receivers must be up and past state-sync BEFORE the sender emits its
    # first op.  We achieve this by:
    #   1. Launching receivers first (they connect to the sender's StateSync
    #      server, which is up as soon as the sender process starts).
    #   2. Sleeping 2 s to let all SSH connections and state-syncs complete.
    #   3. Giving the sender a --start-delay of 5000 ms so it waits an
    #      additional 5 s inside its headless script before sending.
    #
    # Timing budget (conservative for a loaded cluster):
    #   ~1 s  receiver SSH + process startup
    #   ~4 ms state sync on an empty document
    #   ─────────────────────────────────────
    #   sleep 2 + start-delay 5000 ms = 7 s total headroom
    local SENDER_DELAY=5000
    # Receiver drain window: long enough to outlast all sender ops + drain.
    # sender ops take OPS * SLEEP_MS/BATCH ms ≈ OPS * 10 ms at default rate.
    local RECV_DRAIN=$(( OPS * 10 + SENDER_DELAY + 5000 ))

    # Step 1: launch the sender process first so its TCP StateSync server is
    # listening when the receivers try to connect.  The sender script starts
    # with SLEEP SENDER_DELAY, so it won't emit any ops for 5 s.
    local sender_script="${TMP_SCRIPT_DIR}/${label}_peer_0.txt"
    "${SCRIPT_DIR}/automated_typing.sh" \
        --mode sender \
        --ops "$OPS" \
        --peer-id 0 \
        --start-delay "${SENDER_DELAY}" \
        > "$sender_script"

    local sender_log="${out_dir}/peer_0.log"
    local sender_dump="${out_dir}/peer_0_dump.txt"

    echo "  [peer 0 / sender] ${sender_host}  port=${sender_port}  start-delay=${SENDER_DELAY}ms"

    ssh_run "$sender_host" \
        "cd '${REPO_DIR}' && '${BINARY}' \
            --port ${sender_port} \
            --first \
            --headless \
            --script '${sender_script}' \
            --log-path '${sender_log}' > '${sender_dump}'"

    # Step 2: give the sender process 2 s to bind its sockets and start the
    # StateSync server before receivers try to connect.
    echo "  Waiting 2s for sender sockets to bind..."
    sleep 2

    # Step 3: launch all receivers.  By the time the sender's SLEEP SENDER_DELAY
    # expires (~3 s from now), all receivers will be synced and listening.
    for (( i=1; i<n; i++ )); do
        local recv_host="${__lhosts[$i]}"
        local recv_port
        recv_port=$(port_for "$i" "$base")

        local recv_script="${TMP_SCRIPT_DIR}/${label}_peer_${i}.txt"
        "${SCRIPT_DIR}/automated_typing.sh" \
            --mode receiver \
            --start-delay "${RECV_DRAIN}" \
            > "$recv_script"

        local recv_log="${out_dir}/peer_${i}.log"
        local recv_dump="${out_dir}/peer_${i}_dump.txt"

        echo "  [peer ${i} / receiver] ${recv_host}  port=${recv_port}"

        ssh_run "$recv_host" \
            "cd '${REPO_DIR}' && '${BINARY}' \
                --port ${recv_port} \
                --peer ${sender_hostname}:${sender_port} \
                --headless \
                --script '${recv_script}' \
                --log-path '${recv_log}' > '${recv_dump}'"
    done
}

# ---------------------------------------------------------------------------
# Convergence / concurrent shared logic
# ---------------------------------------------------------------------------

_run_writer_experiment() {
    local label="$1"
    local -n __whosts="$2"
    local base="$3"
    local out_dir="$4"
    local start_delay_others="$5"   # 0 for concurrent, 500 for convergence

    local n="${#__whosts[@]}"

    # Pre-compute all hostnames and ports
    declare -a hostnames=()
    declare -a ports=()
    for (( i=0; i<n; i++ )); do
        hostnames+=("$(host_of "${__whosts[$i]}")")
        ports+=("$(port_for "$i" "$base")")
    done

    # Generate scripts for all peers
    for (( i=0; i<n; i++ )); do
        local delay
        if [[ "$i" -eq 0 ]]; then
            delay=0
        else
            delay="$start_delay_others"
        fi

        "${SCRIPT_DIR}/automated_typing.sh" \
            --mode writer \
            --ops "$OPS" \
            --peer-id "$i" \
            --n-peers "$n" \
            --start-delay "$delay" \
            > "${TMP_SCRIPT_DIR}/${label}_peer_${i}.txt"
    done

    # Launch peer 0 first (--first), sleep 1s so it binds before others connect.
    local peer0_host="${__whosts[0]}"
    local peer0_port="${ports[0]}"

    # Build --peer flags for peer 0: all peers j != 0
    local peer0_peers=""
    for (( j=1; j<n; j++ )); do
        peer0_peers+=" --peer ${hostnames[$j]}:${ports[$j]}"
    done

    local p0_log="${out_dir}/peer_0.log"
    local p0_dump="${out_dir}/peer_0_dump.txt"
    local p0_script="${TMP_SCRIPT_DIR}/${label}_peer_0.txt"

    echo "  [peer 0 / first+writer] ${peer0_host}  port=${peer0_port}"

    # shellcheck disable=SC2086
    ssh_run "$peer0_host" \
        "cd '${REPO_DIR}' && '${BINARY}' \
            --port ${peer0_port} \
            --first \
            ${peer0_peers} \
            --headless \
            --script '${p0_script}' \
            --log-path '${p0_log}' > '${p0_dump}'"

    sleep 1

    # Launch peers 1..N-1 simultaneously with full-mesh --peer flags
    for (( i=1; i<n; i++ )); do
        local peer_host="${__whosts[$i]}"
        local peer_port="${ports[$i]}"

        local peer_peers=""
        for (( j=0; j<n; j++ )); do
            [[ "$j" -eq "$i" ]] && continue
            peer_peers+=" --peer ${hostnames[$j]}:${ports[$j]}"
        done

        local pi_log="${out_dir}/peer_${i}.log"
        local pi_dump="${out_dir}/peer_${i}_dump.txt"
        local pi_script="${TMP_SCRIPT_DIR}/${label}_peer_${i}.txt"

        echo "  [peer ${i} / writer] ${peer_host}  port=${peer_port}"

        # shellcheck disable=SC2086
        ssh_run "$peer_host" \
            "cd '${REPO_DIR}' && '${BINARY}' \
                --port ${peer_port} \
                ${peer_peers} \
                --headless \
                --script '${pi_script}' \
                --log-path '${pi_log}' > '${pi_dump}'"
    done
}

_run_convergence() {
    local label="$1"
    local -n __cvhosts="$2"
    local base="$3"
    local out_dir="$4"
    _run_writer_experiment "$label" __cvhosts "$base" "$out_dir" 500
}

_run_concurrent() {
    local label="$1"
    local -n __cchosts="$2"
    local base="$3"
    local out_dir="$4"
    _run_writer_experiment "$label" __cchosts "$base" "$out_dir" 0
}

# ---------------------------------------------------------------------------
# Formal convergence trial loop
#
# Runs TRIALS trials, cycling through MIN_PEERS..MAX_PEERS peers per trial.
# Each peer executes a seeded random mix of INSERT/DELETE ops produced by
# gen_convergence_script.py.  Dumps are collected per trial for byte-for-byte
# comparison during analysis.
# ---------------------------------------------------------------------------

_run_convergence_trials() {
    local base="$1"
    local out_base="${RESULTS_DIR}/convergence"
    mkdir -p "$out_base"
    mkdir -p "${TMP_SCRIPT_DIR}"

    local peer_range=$(( MAX_PEERS - MIN_PEERS + 1 ))
    local passed=0
    local failed=0

    echo ""
    echo "  Running ${TRIALS} convergence trials (${MIN_PEERS}–${MAX_PEERS} peers," \
         "${OPS} ops each, 5 s quiescence)"

    for (( trial=1; trial<=TRIALS; trial++ )); do
        local n_peers=$(( MIN_PEERS + (trial - 1) % peer_range ))
        local seed=$(( SEED_BASE * 1000 + trial ))
        local trial_label
        trial_label="trial_$(printf '%03d' "$trial")"
        local trial_dir="${out_base}/${trial_label}"
        mkdir -p "$trial_dir"

        # Record metadata for reproducibility and post-mortem analysis.
        printf "trial=%d n_peers=%d ops=%d seed=%d min_peers=%d max_peers=%d\n" \
            "$trial" "$n_peers" "$OPS" "$seed" "$MIN_PEERS" "$MAX_PEERS" \
            > "${trial_dir}/meta.txt"

        # Cap hosts to n_peers for this trial.
        local trial_hosts=("${ALL_HOSTS[@]:0:$n_peers}")
        local sender_host="${trial_hosts[0]}"
        local sender_hostname
        sender_hostname=$(host_of "$sender_host")
        local sender_port
        sender_port=$(port_for 0 "$base")

        # Pre-compute hostnames and ports for full-mesh peer flags.
        declare -a t_hostnames=()
        declare -a t_ports=()
        for (( i=0; i<n_peers; i++ )); do
            t_hostnames+=("$(host_of "${trial_hosts[$i]}")")
            t_ports+=("$(port_for "$i" "$base")")
        done

        # Generate a unique random script per peer.
        for (( i=0; i<n_peers; i++ )); do
            local delay=$(( i == 0 ? 0 : 500 ))
            python3 "${SCRIPT_DIR}/gen_convergence_script.py" \
                --ops "$OPS" \
                --peer-id "$i" \
                --seed $(( seed + i * 997 )) \
                --start-delay "$delay" \
                --drain-ms 5000 \
                > "${TMP_SCRIPT_DIR}/${trial_label}_peer_${i}.txt"
        done

        printf "  [%3d/%d] %d peers  seed=%-8d  " \
            "$trial" "$TRIALS" "$n_peers" "$seed"

        # Launch peer 0 (--first) with --peer flags for all others.
        local p0_peers=""
        for (( j=1; j<n_peers; j++ )); do
            p0_peers+=" --peer ${t_hostnames[$j]}:${t_ports[$j]}"
        done
        local p0_log="${trial_dir}/peer_0.log"
        local p0_dump="${trial_dir}/peer_0_dump.txt"
        local p0_script="${TMP_SCRIPT_DIR}/${trial_label}_peer_0.txt"

        # shellcheck disable=SC2086
        ssh_run "${trial_hosts[0]}" \
            "cd '${REPO_DIR}' && '${BINARY}' \
                --port ${t_ports[0]} --first ${p0_peers} \
                --headless --script '${p0_script}' \
                --log-path '${p0_log}' > '${p0_dump}'"

        sleep 1  # let peer 0 bind before others connect

        # Launch peers 1..N-1 with full-mesh --peer flags.
        for (( i=1; i<n_peers; i++ )); do
            local pi_peers=""
            for (( j=0; j<n_peers; j++ )); do
                [[ "$j" -eq "$i" ]] && continue
                pi_peers+=" --peer ${t_hostnames[$j]}:${t_ports[$j]}"
            done
            local pi_log="${trial_dir}/peer_${i}.log"
            local pi_dump="${trial_dir}/peer_${i}_dump.txt"
            local pi_script="${TMP_SCRIPT_DIR}/${trial_label}_peer_${i}.txt"

            # shellcheck disable=SC2086
            ssh_run "${trial_hosts[$i]}" \
                "cd '${REPO_DIR}' && '${BINARY}' \
                    --port ${t_ports[$i]} ${pi_peers} \
                    --headless --script '${pi_script}' \
                    --log-path '${pi_log}' > '${pi_dump}'"
        done

        # Wait for all peers to finish.
        for pid in "${_BGPIDS[@]}"; do
            wait "$pid" 2>/dev/null || true
        done
        _BGPIDS=()

        # Byte-for-byte comparison of dump files.
        local reference=""
        local trial_ok=1
        for (( i=0; i<n_peers; i++ )); do
            local dump="${trial_dir}/peer_${i}_dump.txt"
            if [[ ! -f "$dump" ]]; then
                echo "MISSING_DUMP"
                trial_ok=0
                break
            fi
            local content
            content=$(cat "$dump")
            if [[ -z "$reference" ]]; then
                reference="$content"
            elif [[ "$content" != "$reference" ]]; then
                trial_ok=0
            fi
        done

        if [[ "$trial_ok" -eq 1 ]]; then
            echo "PASS"
            passed=$(( passed + 1 ))
        else
            echo "FAIL  ← logs: ${trial_dir}/"
            failed=$(( failed + 1 ))
            # Record which peers differed for post-mortem.
            {
                echo "CONVERGENCE FAILURE — trial ${trial}"
                echo "n_peers=${n_peers}  seed=${seed}  ops=${OPS}"
                echo ""
                for (( i=0; i<n_peers; i++ )); do
                    echo "--- peer_${i}_dump.txt ---"
                    cat "${trial_dir}/peer_${i}_dump.txt" 2>/dev/null || echo "(missing)"
                    echo ""
                done
            } > "${trial_dir}/divergence_report.txt"
        fi
    done

    echo ""
    echo "  ┌──────────────────────────────────────┐"
    printf  "  │  Trials: %-3d   PASS: %-3d   FAIL: %-3d │\n" \
        "$TRIALS" "$passed" "$failed"
    echo "  └──────────────────────────────────────┘"
    echo "  Full results: ${out_base}/"
}

# ---------------------------------------------------------------------------
# Scalability experiment
#
# Tests 2, 4, 6, 8, and 10 concurrent peers.  Peers are assigned to cluster
# hosts round-robin so experiments beyond the host count run multiple peers
# per node (2 per host maximum with the default 5-host cluster).
#
# Each peer writes 100 ops at 5 ops/sec (batch=5, sleep-ms=1000).
# CPU time is captured via /usr/bin/time -f for each peer process.
# ---------------------------------------------------------------------------

_run_scalability() {
    local base="$1"
    local out_base="${RESULTS_DIR}/scalability"
    mkdir -p "$out_base"
    mkdir -p "${TMP_SCRIPT_DIR}"

    local n_hosts="${#ALL_HOSTS[@]}"

    # Peer counts to test.  Skip any that need more than 2x the host count.
    local -a SCALE_COUNTS=(2 4 6 8 10)

    echo ""
    echo "  Scalability sweep: peer counts ${SCALE_COUNTS[*]}"
    echo "  Hosts available  : ${n_hosts}"
    echo "  Ops per peer     : 100  (5 ops/sec)"

    for n_peers in "${SCALE_COUNTS[@]}"; do
        # Require at least ceil(n_peers/2) hosts.
        local hosts_needed=$(( (n_peers + 1) / 2 ))
        if [[ "$n_hosts" -lt "$hosts_needed" ]]; then
            echo "  Skipping ${n_peers}-peer: need ${hosts_needed} hosts, only ${n_hosts} available."
            continue
        fi

        local label="${n_peers}peer"
        local out_dir="${out_base}/${label}"
        mkdir -p "$out_dir"

        echo ""
        echo "  --- ${n_peers} peers ---"

        # Assign peers to hosts round-robin; compute ports.
        local -a t_hostnames=()
        local -a t_ports=()
        for (( i=0; i<n_peers; i++ )); do
            local hidx=$(( i % n_hosts ))
            t_hostnames+=("$(host_of "${ALL_HOSTS[$hidx]}")")
            t_ports+=("$(port_for "$i" "$base")")
        done

        # Generate per-peer random scripts: 100 ops, 5 ops/sec, 5 s quiescence.
        for (( i=0; i<n_peers; i++ )); do
            local delay=$(( i == 0 ? 0 : 500 ))
            python3 "${SCRIPT_DIR}/gen_convergence_script.py" \
                --ops 100 \
                --peer-id "$i" \
                --seed $(( 77777 + i * 997 )) \
                --start-delay "$delay" \
                --batch 5 \
                --sleep-ms 1000 \
                --drain-ms 5000 \
                > "${TMP_SCRIPT_DIR}/scale_${n_peers}p_peer_${i}.txt"
        done

        # Launch peer 0 (--first) with --peer flags for all others.
        local p0_peers=""
        for (( j=1; j<n_peers; j++ )); do
            p0_peers+=" --peer ${t_hostnames[$j]}:${t_ports[$j]}"
        done
        local p0_log="${out_dir}/peer_0.log"
        local p0_dump="${out_dir}/peer_0_dump.txt"
        local p0_cpu="${out_dir}/cpu_0.txt"
        local p0_script="${TMP_SCRIPT_DIR}/scale_${n_peers}p_peer_0.txt"

        # shellcheck disable=SC2086
        ssh_run "${ALL_HOSTS[0]}" \
            "cd '${REPO_DIR}' && \
             { /usr/bin/time -f 'cpu_pct=%P wall_sec=%e' \
               '${BINARY}' --port ${t_ports[0]} --first ${p0_peers} \
               --headless --script '${p0_script}' \
               --log-path '${p0_log}' > '${p0_dump}'; \
             } 2>'${p0_cpu}'"

        sleep 1  # let peer 0 bind before others connect

        # Launch peers 1..N-1 with full-mesh --peer flags.
        for (( i=1; i<n_peers; i++ )); do
            local hidx=$(( i % n_hosts ))
            local peer_host="${ALL_HOSTS[$hidx]}"

            local pi_peers=""
            for (( j=0; j<n_peers; j++ )); do
                [[ "$j" -eq "$i" ]] && continue
                pi_peers+=" --peer ${t_hostnames[$j]}:${t_ports[$j]}"
            done
            local pi_log="${out_dir}/peer_${i}.log"
            local pi_dump="${out_dir}/peer_${i}_dump.txt"
            local pi_cpu="${out_dir}/cpu_${i}.txt"
            local pi_script="${TMP_SCRIPT_DIR}/scale_${n_peers}p_peer_${i}.txt"

            # shellcheck disable=SC2086
            ssh_run "$peer_host" \
                "cd '${REPO_DIR}' && \
                 { /usr/bin/time -f 'cpu_pct=%P wall_sec=%e' \
                   '${BINARY}' --port ${t_ports[$i]} ${pi_peers} \
                   --headless --script '${pi_script}' \
                   --log-path '${pi_log}' > '${pi_dump}'; \
                 } 2>'${pi_cpu}'"
        done

        # Wait for all peers to finish.
        for pid in "${_BGPIDS[@]}"; do
            wait "$pid" 2>/dev/null || true
        done
        _BGPIDS=()

        echo "    ${n_peers}-peer done. Logs: ${out_dir}/"
        sleep 2  # brief gap before next experiment to let ports drain
    done

    echo ""
    echo "  Scalability sweep complete. Results: ${out_base}/"
}

# ---------------------------------------------------------------------------
# Dispatch experiments by eval type
# ---------------------------------------------------------------------------

mkdir -p "${RESULTS_DIR}"

case "$EVAL_TYPE" in
    latency)
        # 2-peer experiment (always)
        HOSTS_2=("${ALL_HOSTS[@]:0:2}")
        run_experiment "2peer" HOSTS_2 "$BASE_PORT"
        sleep 2

        # 3-peer experiment (if enough hosts)
        if [[ "${#ALL_HOSTS[@]}" -ge 3 ]]; then
            HOSTS_3=("${ALL_HOSTS[@]:0:3}")
            run_experiment "3peer" HOSTS_3 "$BASE_PORT"
            sleep 2
        else
            echo ""
            echo "Skipping 3-peer: need 3 hosts, only ${#ALL_HOSTS[@]} configured."
        fi

        # 4-peer experiment (if enough hosts)
        if [[ "${#ALL_HOSTS[@]}" -ge 4 ]]; then
            HOSTS_4=("${ALL_HOSTS[@]:0:4}")
            run_experiment "4peer" HOSTS_4 "$BASE_PORT"
            sleep 2
        else
            echo ""
            echo "Skipping 4-peer: need 4 hosts, only ${#ALL_HOSTS[@]} configured."
        fi

        # 5-peer experiment (if enough hosts)
        if [[ "${#ALL_HOSTS[@]}" -ge 5 ]]; then
            HOSTS_5=("${ALL_HOSTS[@]:0:5}")
            run_experiment "5peer" HOSTS_5 "$BASE_PORT"
        else
            echo ""
            echo "Skipping 5-peer: need 5 hosts, only ${#ALL_HOSTS[@]} configured."
        fi
        ;;

    convergence)
        # Validate cluster has enough hosts for the requested max peers.
        if [[ "${#ALL_HOSTS[@]}" -lt "$MAX_PEERS" ]]; then
            echo "ERROR: --max-peers ${MAX_PEERS} requires ${MAX_PEERS} hosts;" \
                 "only ${#ALL_HOSTS[@]} configured in ${CLUSTER_CONF}" >&2
            exit 1
        fi
        _run_convergence_trials "$BASE_PORT"
        ;;

    concurrent)
        N="${#ALL_HOSTS[@]}"
        HOSTS_CC=("${ALL_HOSTS[@]}")
        run_experiment "concurrent_${N}peer" HOSTS_CC "$BASE_PORT"
        ;;

    scalability)
        _run_scalability "$BASE_PORT"
        ;;
esac

# ---------------------------------------------------------------------------
# Analyze
# ---------------------------------------------------------------------------

if [[ "$RUN_ANALYZE" -eq 1 ]]; then
    echo ""
    echo "================================================================="
    echo "  Running analysis"
    echo "================================================================="
    if [[ "$EVAL_TYPE" == "scalability" ]]; then
        python3 "${SCRIPT_DIR}/analyze_scalability.py" \
            "${RESULTS_DIR}/scalability" || true
    else
        python3 "${SCRIPT_DIR}/analyze_results.py" "${RESULTS_DIR}" || true
    fi
fi
