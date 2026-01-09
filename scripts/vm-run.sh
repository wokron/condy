#!/bin/bash

set -euo pipefail

DEFAULT_COMMAND="/bin/sh"

usage() {
    echo "Usage: $0 [-hkq] [-c <cmd>] <bzImage> [file]..."
    echo "Run a VM with the specified kernel image and static executable."
    echo "  -h                   Show this help message."
    echo "  -k                   Enable KVM acceleration."
    echo "  -q                   Quiet mode (no kernel messages)."
    echo "  -c <cmd>             Command to run inside the VM. Default is '$DEFAULT_COMMAND'."
    echo "  <bzImage>            Path to the kernel image."
    echo "  [file]...            One or more files to include in the initrd."
    echo "                       Static executable is required to run inside the VM."
    exit 1
}

KVM_ENABLED=false
QUIET_MODE=false
COMMAND="$DEFAULT_COMMAND"

while getopts "hkqc:" opt; do
    case $opt in
        k)
            KVM_ENABLED=true
            ;;
        q)
            QUIET_MODE=true
            ;;
        c)
            COMMAND="$OPTARG"
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

if [ $# -lt 1 ]; then
    usage
fi

KERNEL_IMAGE="$1"
FILES="${@:2}"

if [ ! -f "$KERNEL_IMAGE" ]; then
    echo "Error: Kernel image '$KERNEL_IMAGE' not found."
    exit 1
fi

for FILE in $FILES; do
    if [ ! -f "$FILE" ]; then
        echo "Error: File '$FILE' not found."
        exit 1
    fi
done

SELF_SCRIPT_DIR=$(dirname "$(realpath "$0")")
LIGHT_INITRD_SCRIPT="$SELF_SCRIPT_DIR/light-initrd.sh"

# Cached initrd output filename
INITRD_OUTPUT="vm-run-initrd.cpio.gz"

TEMP_DIR="$SELF_SCRIPT_DIR/.temp"
mkdir -p "$TEMP_DIR"
trap "rm -rf $TEMP_DIR" EXIT

cat > "$TEMP_DIR/init.sh" <<EOF
#!/bin/sh
$COMMAND
EOF
chmod +x "$TEMP_DIR/init.sh"

echo "Building initrd image..."
bash "$LIGHT_INITRD_SCRIPT" -o "$TEMP_DIR/$INITRD_OUTPUT" -s "/root/init.sh" $FILES "$TEMP_DIR/init.sh"
echo "Initrd image created at $TEMP_DIR/$INITRD_OUTPUT"

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
    -initrd "$TEMP_DIR/$INITRD_OUTPUT" \
    -append "$KERNEL_ARGS" \
    -nographic
