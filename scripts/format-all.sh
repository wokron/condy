#!/bin/bash

set -euo pipefail

usage() {
    echo "Usage: $0 [-i]"
    echo "Format all C++ source files in the project."
    echo "  -h    Show this help message."
    echo "  -i    Apply formatting changes in place. If not set, performs a dry run."
    exit 1
}

IN_PLACE=0

while getopts "ih" opt; do
    case $opt in
        i)
            IN_PLACE=1
            ;;
        h)
            usage
            ;;
        *)
            usage
            ;;
    esac
done

ALL_FILES=$(find ./ \( -name "*.cpp" -o -name "*.hpp" \) -type f \
        -not -path "./third_party/*" -not -path "./build/*")

if [ $IN_PLACE -eq 1 ]; then
    echo "Formatting files..."
    clang-format -i $ALL_FILES
    echo "Complete."
else
    clang-format --dry-run --Werror $ALL_FILES
    if [ $? -eq 0 ]; then
        echo "Nothing to format."
    fi
fi
