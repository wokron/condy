#!/bin/bash
set -e

usage() {
    echo "Usage: $0 [-t TIMEOUT] <directory>"
    echo "  -t TIMEOUT  Timeout in seconds for each executable (default: 60)"
    exit 1
}

TIMEOUT=60

while getopts "t:" opt; do
    case $opt in
        t) TIMEOUT=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))

DIR=${1:?$(usage)}

if [ ! -d "$DIR" ]; then
    echo "Error: '$DIR' is not a directory"
    exit 1
fi

for EXEC in "$DIR"/*; do
    [ -f "$EXEC" ] && [ -x "$EXEC" ] || continue
    NAME=$(basename "$EXEC")
    echo "--- Running: $NAME ---"
    timeout "${TIMEOUT}s" "$EXEC"
done
