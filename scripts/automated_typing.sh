#!/usr/bin/env bash
# automated_typing.sh — Generate a headless command script for p2p-editor.
#
# Prints the script to stdout; redirect to a file and pass with --script.
#
# Usage:
#   scripts/automated_typing.sh [OPTIONS]
#
# Options:
#   --mode sender|receiver|writer  Role (default: writer)
#   --ops N                        Number of INSERT operations (default: 1000)
#   --peer-id N                    0-based peer index, offsets char set (default: 0)
#   --n-peers N                    Total peers (default: 1)
#   --start-delay MS               Initial SLEEP before any ops (default: 5000)
#   --batch N                      INSERTs per sleep interval (default: 10)
#   --sleep-ms N                   ms to sleep between batches (default: 10)
#   --drain-ms N                   Final sleep before DUMP (default: 3000)
#   --with-deletes                 Interleave one DELETE 0 per batch (after first)
#   --no-dump                      Suppress the final DUMP line

set -euo pipefail

MODE="writer"
OPS=1000
PEER_ID=0
N_PEERS=1
START_DELAY=5000
BATCH=10
SLEEP_MS=10
DRAIN_MS=3000
WITH_DELETES=0
NO_DUMP=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)         MODE="$2";         shift 2 ;;
        --ops)          OPS="$2";          shift 2 ;;
        --peer-id)      PEER_ID="$2";      shift 2 ;;
        --n-peers)      N_PEERS="$2";      shift 2 ;;
        --start-delay)  START_DELAY="$2";  shift 2 ;;
        --batch)        BATCH="$2";        shift 2 ;;
        --sleep-ms)     SLEEP_MS="$2";     shift 2 ;;
        --drain-ms)     DRAIN_MS="$2";     shift 2 ;;
        --with-deletes) WITH_DELETES=1;    shift   ;;
        --no-dump)      NO_DUMP=1;         shift   ;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "ERROR: unknown argument '$1'" >&2; exit 1 ;;
    esac
done

# Characters cycle a-z, offset per peer so concurrent writers produce distinct chars.
CHAR_OFFSET=$(( (PEER_ID * 7) % 26 ))

emit_receiver() {
    echo "SLEEP ${START_DELAY}"
    [[ "$NO_DUMP" -eq 0 ]] && echo "DUMP"
    echo "QUIT"
}

emit_writer() {
    echo "SLEEP ${START_DELAY}"

    local inserted=0
    local batch_num=0
    # pos tracks the next insert position (end of the writer's own chars).
    # doc_len also tracks deletions so we never DELETE from an empty doc.
    local pos=0
    local doc_len=0

    while (( inserted < OPS )); do
        local this_batch=$(( OPS - inserted ))
        (( this_batch > BATCH )) && this_batch=$BATCH

        for (( b=0; b<this_batch; b++ )); do
            local char_idx=$(( (inserted + CHAR_OFFSET) % 26 ))
            # printf char: 'a' is ASCII 97
            local ch
            ch=$(printf "\\x$(printf '%02x' $(( 97 + char_idx )) )")
            echo "INSERT ${pos} ${ch}"
            inserted=$(( inserted + 1 ))
            pos=$(( pos + 1 ))
            doc_len=$(( doc_len + 1 ))
        done

        # Interleave one DELETE per batch after the first batch, but only if doc non-empty.
        if [[ "$WITH_DELETES" -eq 1 && "$batch_num" -gt 0 && "$doc_len" -gt 0 ]]; then
            echo "DELETE 0"
            pos=$(( pos - 1 ))
            doc_len=$(( doc_len - 1 ))
        fi

        batch_num=$(( batch_num + 1 ))

        # Sleep between batches, but skip trailing sleep if we just finished.
        if (( inserted < OPS )); then
            echo "SLEEP ${SLEEP_MS}"
        fi
    done

    echo "SLEEP ${DRAIN_MS}"
    [[ "$NO_DUMP" -eq 0 ]] && echo "DUMP"
    echo "QUIT"
}

case "$MODE" in
    receiver)
        emit_receiver
        ;;
    sender|writer)
        emit_writer
        ;;
    *)
        echo "ERROR: unknown mode '${MODE}' (expected sender, receiver, or writer)" >&2
        exit 1
        ;;
esac
