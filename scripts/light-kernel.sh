#!/bin/bash

set -euo pipefail

DEFAULT_OUTPUT_BZIMAGE="./bzImage"

usage() {
    echo "Usage: $0 [-ho] <kernel-archive-url>"
    echo "Build a lightweight Linux kernel suitable for running in a VM."
    echo "  <kernel-archive-url>  URL to download the kernel source archive."
    echo "  -o <output-bzImage>   Path to output the built bzImage. Default is '$DEFAULT_OUTPUT_BZIMAGE'."
    echo "  -h                    Show this help message."
    exit 1
}

OUTPUT_BZIMAGE="$DEFAULT_OUTPUT_BZIMAGE"

while getopts ":ho:" opt; do
    case ${opt} in
        h )
            usage
            ;;
        o )
            OUTPUT_BZIMAGE="$OPTARG"
            ;;
        * )
            usage
            ;;
    esac
done
shift $((OPTIND -1))

if [ $# -ne 1 ]; then
    usage
fi

KERNEL_ARCHIVE_URL="$1"

# Create a temporary directory for building the kernel
BUILD_DIR=$(mktemp -d)
trap 'rm -rf "$BUILD_DIR"' EXIT

# Download and extract the kernel source
KERNEL_ARCHIVE_NAME=$(basename "$KERNEL_ARCHIVE_URL")
wget -P "$BUILD_DIR" "$KERNEL_ARCHIVE_URL"
tar -xf "$BUILD_DIR/$KERNEL_ARCHIVE_NAME" -C "$BUILD_DIR"

SOURCE_DIR_NAME="${KERNEL_ARCHIVE_NAME%.tar.*}"
SOURCE_DIR="$BUILD_DIR/$SOURCE_DIR_NAME"

REALPATH_OUTPUT_BZIMAGE=$(realpath "$OUTPUT_BZIMAGE")

cd "$SOURCE_DIR"

# Configure default settings
make defconfig
# Configure kvm guest support
make kvm_guest.config

# Build the kernel
make -j"$(nproc)" bzImage

# Copy the built kernel image to the specified output path
cp "arch/x86/boot/bzImage" "$REALPATH_OUTPUT_BZIMAGE"
echo "Kernel built successfully at '$REALPATH_OUTPUT_BZIMAGE'"