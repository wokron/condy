#!/bin/bash

set -euo pipefail


usage() {
    echo "Usage: $0 [-hk] <bzImage> <static-executable>"
    echo "Run a VM with the specified kernel image and static executable."
    echo "  -h                   Show this help message."
    echo "  -k                   Enable KVM acceleration."
    echo "  -q                   Quiet mode (no output)."
    echo "  <bzImage>            Path to the kernel image."
    echo "  <static-executable>  Path to the static executable to run inside the VM."
    exit 1
}

KVM_ENABLED=false
QUIET_MODE=false

while getopts "hk" opt; do
    case $opt in
        k)
            KVM_ENABLED=true
            ;;
        q)
            QUIET_MODE=true
            ;;
        h)
            usage
            ;;
        *)
            usage
            ;;
    esac
done
shift $((OPTIND -1))

if [ $# -ne 2 ]; then
    usage
fi

KERNEL_IMAGE="$1"
STATIC_EXECUTABLE="$2"

if [ ! -f "$KERNEL_IMAGE" ]; then
    echo "Error: Kernel image '$KERNEL_IMAGE' not found."
    exit 1
fi

if [ ! -f "$STATIC_EXECUTABLE" ]; then
    echo "Error: Static executable '$STATIC_EXECUTABLE' not found."
    exit 1
fi

SELF_SCRIPT_DIR=$(dirname "$(realpath "$0")")
LIGHT_INITRD_SCRIPT="$SELF_SCRIPT_DIR/light-initrd.sh"

# MD5 of the static executable
STATIC_EXECUTABLE_MD5=$(md5sum "$STATIC_EXECUTABLE" | awk '{print $1}')

# Cached initrd output filename
INITRD_OUTPUT="initrd-${STATIC_EXECUTABLE_MD5}.cpio.gz"

CACHE_DIR="$SELF_SCRIPT_DIR/.cache"
mkdir -p "$CACHE_DIR"

if [ ! -f "$CACHE_DIR/$INITRD_OUTPUT" ]; then
    # Clear previous cached initrd images
    rm -f "$CACHE_DIR"/initrd-*.cpio.gz

    echo "Building initrd image..."
    bash "$LIGHT_INITRD_SCRIPT" -o "$CACHE_DIR/$INITRD_OUTPUT" -s "/root/$(basename "$STATIC_EXECUTABLE")" "$STATIC_EXECUTABLE"
    echo "Initrd image created at $CACHE_DIR/$INITRD_OUTPUT"
else
    echo "Using cached initrd image at $CACHE_DIR/$INITRD_OUTPUT"
fi

KVM_FLAG=""
if [ "$KVM_ENABLED" = true ]; then
    KVM_FLAG="-enable-kvm"
fi

KERNEL_ARGS="console=ttyS0"
if [ "$QUIET_MODE" = true ]; then
    KERNEL_ARGS="$KERNEL_ARGS quiet"
fi

# Run the VM,
qemu-system-x86_64 \
    $KVM_FLAG \
    -m 512M \
    -kernel "$KERNEL_IMAGE" \
    -initrd "$CACHE_DIR/$INITRD_OUTPUT" \
    -append "$KERNEL_ARGS" \
    -nographic
