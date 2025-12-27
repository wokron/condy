#!/bin/bash

set -euo pipefail

usage() {
    echo "Usage: $0 [-hf]"
    echo "Run clang-tidy on all source files in this project."
    echo "  -h    Show this help message."
    exit 1
}

while getopts "h" opt; do
    case $opt in
        h)
            usage
            ;;
        *)
            usage
            ;;
    esac
done

run-clang-tidy -p build/ '^.*(benchmarks|examples|tests)/.*\.(hpp|cpp)$'
