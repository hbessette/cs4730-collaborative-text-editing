#!/usr/bin/env bash
# collect_results.sh — Archive evaluation results and optionally SCP from nodes.
#
# Usage:
#   scripts/collect_results.sh [OPTIONS]
#
# Options:
#   --results DIR   Results root directory (default: logs/)
#   --archive FILE  Output .tar.gz path (default: logs/eval_<timestamp>.tar.gz)
#   --scp           SCP logs from each host instead of assuming shared FS
#   --config FILE   Cluster config file (default: scripts/cluster.conf)
#   -h|--help       Show this help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

RESULTS_DIR="${REPO_DIR}/logs"
ARCHIVE=""
DO_SCP=0
CLUSTER_CONF="${SCRIPT_DIR}/cluster.conf"

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --results)  RESULTS_DIR="$2"; shift 2 ;;
        --archive)  ARCHIVE="$2";     shift 2 ;;
        --scp)      DO_SCP=1;         shift   ;;
        --config)   CLUSTER_CONF="$2"; shift 2 ;;
        -h|--help)  usage; exit 0     ;;
        *) echo "ERROR: unknown argument '$1'" >&2; usage; exit 1 ;;
    esac
done

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
if [[ -z "$ARCHIVE" ]]; then
    ARCHIVE="${RESULTS_DIR}/eval_${TIMESTAMP}.tar.gz"
fi

# ---------------------------------------------------------------------------
# SCP mode: pull logs from each cluster host
# ---------------------------------------------------------------------------

if [[ "$DO_SCP" -eq 1 ]]; then
    if [[ ! -f "$CLUSTER_CONF" ]]; then
        echo "ERROR: cluster config not found at '${CLUSTER_CONF}'" >&2
        exit 1
    fi

    declare -a HOSTS=()
    while IFS= read -r line; do
        line="${line%%#*}"
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [[ -z "$line" ]] && continue
        HOSTS+=("$line")
    done < "$CLUSTER_CONF"

    echo "SCP mode: pulling logs from ${#HOSTS[@]} host(s)..."
    mkdir -p "${RESULTS_DIR}"

    for host in "${HOSTS[@]}"; do
        echo "  Fetching from ${host}..."
        # rsync is preferred; fall back to scp -r if rsync unavailable.
        if command -v rsync &>/dev/null; then
            rsync -az \
                -e "ssh -o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=10" \
                "${host}:${RESULTS_DIR}/" \
                "${RESULTS_DIR}/" \
                2>/dev/null || echo "  WARNING: rsync from ${host} failed or no files found"
        else
            scp \
                -o StrictHostKeyChecking=no \
                -o BatchMode=yes \
                -o ConnectTimeout=10 \
                -r \
                "${host}:${RESULTS_DIR}/." \
                "${RESULTS_DIR}/" \
                2>/dev/null || echo "  WARNING: scp from ${host} failed or no files found"
        fi
    done
fi

# ---------------------------------------------------------------------------
# Create archive
# ---------------------------------------------------------------------------

if [[ ! -d "$RESULTS_DIR" ]]; then
    echo "ERROR: results directory '${RESULTS_DIR}' does not exist" >&2
    exit 1
fi

# Collect only log and dump files (skip existing archives and tmp scripts).
mapfile -t LOG_FILES < <(
    find "${RESULTS_DIR}" \
        -not -path "${RESULTS_DIR}/.tmp_scripts/*" \
        \( -name "*.log" -o -name "*_dump.txt" \) \
        2>/dev/null | sort
)

if [[ "${#LOG_FILES[@]}" -eq 0 ]]; then
    echo "WARNING: no .log or _dump.txt files found under '${RESULTS_DIR}'" >&2
fi

# Discover subdirectories that contain results
mapfile -t SUBDIRS < <(
    find "${RESULTS_DIR}" \
        -mindepth 1 -maxdepth 1 \
        -type d \
        -not -name ".tmp_scripts" \
        2>/dev/null | sort
)

mkdir -p "$(dirname "${ARCHIVE}")"

# Build archive from the results dir, filtering to log/dump files.
tar -czf "${ARCHIVE}" \
    -C "$(dirname "${RESULTS_DIR}")" \
    --exclude='.tmp_scripts' \
    --exclude='*.tar.gz' \
    "$(basename "${RESULTS_DIR}")" \
    2>/dev/null || {
    echo "WARNING: tar encountered errors (some files may be missing)" >&2
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo ""
echo "================================================================="
echo "  Results Summary"
echo "================================================================="

echo "  Directories collected:"
if [[ "${#SUBDIRS[@]}" -gt 0 ]]; then
    for d in "${SUBDIRS[@]}"; do
        local_count=$(find "$d" \( -name "*.log" -o -name "*_dump.txt" \) 2>/dev/null | wc -l)
        echo "    ${d}  (${local_count} files)"
    done
else
    echo "    (none)"
fi

total_count="${#LOG_FILES[@]}"
if [[ "$total_count" -gt 0 ]]; then
    total_size=$(du -sh "${RESULTS_DIR}" --exclude='.tmp_scripts' 2>/dev/null | cut -f1 || echo "unknown")
    echo "  Total files : ${total_count}"
    echo "  Total size  : ${total_size}"
else
    echo "  Total files : 0"
fi

if [[ -f "$ARCHIVE" ]]; then
    archive_size=$(du -sh "${ARCHIVE}" 2>/dev/null | cut -f1 || echo "unknown")
    echo "  Archive     : ${ARCHIVE}  (${archive_size})"
else
    echo "  Archive     : (not created — no files found)"
fi
echo ""
