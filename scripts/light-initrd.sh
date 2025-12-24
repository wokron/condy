#!/bin/bash

set -euo pipefail

DEFAULT_OUTPUT_FILE="light-initrd.cpio.gz"
DEFAULT_SHELL="/bin/sh"
DEFAULT_DEST_DIR="/root"

usage() {
    echo "Usage: $0 [-ho] [file]..."
    echo "Build a minimal initrd image."
    echo "  -h    Show this help message."
    echo "  -o    Specify output initrd file name. Default is '$DEFAULT_OUTPUT_FILE'."
    echo "  -s    Specify the program to run on startup. Default is '$DEFAULT_SHELL'."
    echo "  -d    Specify the destination directory for included files. Default is '$DEFAULT_DEST_DIR'."
    echo "  file  One or more files to include in the initrd."
    exit 1
}

OUTPUT_FILE="$DEFAULT_OUTPUT_FILE"
SHELL="$DEFAULT_SHELL"
DEST_DIR="$DEFAULT_DEST_DIR"

while getopts "ho:s:d:" opt; do
    case $opt in
        h)
            usage
            ;;
        o)
            OUTPUT_FILE="$OPTARG"
            ;;
        s)
            SHELL="$OPTARG"
            ;;
        d)
            DEST_DIR="$OPTARG"
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

FILES="$*"

WORK_DIR=$(mktemp -d)
trap "rm -rf $WORK_DIR" EXIT

mkdir -p "$WORK_DIR/bin"

# Copy busybox into the work directory
BUSYBOX_PATH=$(which busybox)
if [ -z "$BUSYBOX_PATH" ]; then
    echo "Error: busybox not found in PATH."
    exit 1
fi
cp "$BUSYBOX_PATH" "$WORK_DIR/bin/busybox"

# Create init script
cat << 'EOF' > "$WORK_DIR/init"
#!/bin/busybox sh

# Initialize minimal directories
busybox mkdir -p /etc /proc /root /sbin /sys /usr/bin /usr/sbin

# Mount necessary filesystems
busybox mount -t proc proc /proc
busybox mount -t sysfs sys /sys
busybox mdev -s

# Install busybox applets
busybox --install -s

# Create a minimal passwd file
echo root::0:0:root:/root:SHELL > /etc/passwd

# Set up loopback interface
hostname localhost
ip link set lo up

# Reduce kernel printk verbosity
echo 5 > /proc/sys/kernel/printk

# Start an interactive shell
login root

# Power off
poweroff -f
EOF
sed -i "s|SHELL|$SHELL|g" "$WORK_DIR/init"
chmod +x "$WORK_DIR/init"

# Copy user-specified files into /root
mkdir -p "$WORK_DIR/root"
for FILE in $FILES; do
    BASENAME=$(basename "$FILE")
    cp "$FILE" "$WORK_DIR/$DEST_DIR/$BASENAME"
done

# Create the initrd image
OUTPUT_FILE_REAL=$(realpath "$OUTPUT_FILE")
cd "$WORK_DIR"
find . | cpio -o -H newc -R 0:0 | gzip -9 > "$OUTPUT_FILE_REAL"
echo "Created initrd image: $OUTPUT_FILE"
