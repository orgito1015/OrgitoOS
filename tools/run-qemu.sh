#!/bin/bash
#
# OrgitoOS QEMU Launcher Script
# Runs the OrgitoOS kernel in QEMU emulator via bootable ISO
#

# Color output for better visibility
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Paths
KERNEL_BIN="build/orgitoos.bin"
ISO_FILE="orgitoos.iso"

# Check if QEMU is installed
if ! command -v qemu-system-i386 &> /dev/null; then
    echo -e "${RED}Error: qemu-system-i386 not found${NC}"
    echo "Please install QEMU:"
    echo "  Ubuntu/Debian: sudo apt-get install qemu-system-x86"
    echo "  Arch Linux:    sudo pacman -S qemu-system-x86"
    echo "  macOS:         brew install qemu"
    exit 1
fi

# Check if the kernel binary exists
if [ ! -f "$KERNEL_BIN" ]; then
    echo -e "${RED}Error: Kernel binary not found at $KERNEL_BIN${NC}"
    echo "Please run 'make' first to build the kernel."
    exit 1
fi

# Build ISO if it doesn't exist or is older than the kernel
if [ ! -f "$ISO_FILE" ] || [ "$KERNEL_BIN" -nt "$ISO_FILE" ]; then
    echo -e "${YELLOW}Building bootable ISO...${NC}"
    
    # Check if ISO creation script exists and is executable
    if [ ! -f tools/create-iso.sh ]; then
        echo -e "${RED}Error: tools/create-iso.sh not found${NC}"
        echo "The ISO creation script is missing from the repository."
        exit 1
    fi
    
    if [ ! -x tools/create-iso.sh ]; then
        chmod +x tools/create-iso.sh 2>/dev/null || {
            echo -e "${RED}Error: Cannot make tools/create-iso.sh executable${NC}"
            echo "Please run: chmod +x tools/create-iso.sh"
            exit 1
        }
    fi
    
    # Run ISO creation, suppress verbose output but capture errors
    ISO_LOG=$(mktemp)
    if ! ./tools/create-iso.sh > "$ISO_LOG" 2>&1; then
        echo -e "${RED}Error: Failed to create ISO${NC}"
        echo ""
        echo "Output from create-iso.sh:"
        cat "$ISO_LOG"
        echo ""
        echo "Required tools: grub-mkrescue, xorriso, mtools"
        echo "Install with:"
        echo "  Ubuntu/Debian: sudo apt-get install grub-pc-bin xorriso mtools"
        echo "  Arch Linux:    sudo pacman -S grub xorriso mtools"
        rm -f "$ISO_LOG"
        exit 1
    fi
    rm -f "$ISO_LOG"
fi

# Display startup info
echo -e "${GREEN}Starting OrgitoOS in QEMU...${NC}"
echo "Kernel: $KERNEL_BIN"
echo "Boot method: ISO with GRUB (compatible with all QEMU versions)"
echo "Press Ctrl+Alt+G to release mouse/keyboard from QEMU"
echo "Press Ctrl+C in terminal to quit"
echo ""

# Launch QEMU with ISO
# Using ISO boot is more reliable than direct kernel boot
# and works with all QEMU versions (including 7.0+)
# -boot d specifies boot from CD-ROM first, avoiding disk read attempts
# -display gtk,zoom-to-fit=on scales the emulated screen to fill the
# window (native VGA text mode is a tiny fixed 720x400px), so maximizing
# or resizing the QEMU window actually makes the display bigger instead
# of leaving it pinned at native size.
# disk.img is the persistent hard disk (OrgitoFS, mounted at /mnt).
DISK_IMG="disk.img"
if [ ! -f "$DISK_IMG" ]; then
    echo -e "${YELLOW}Creating 32 MiB disk image $DISK_IMG...${NC}"
    dd if=/dev/zero of="$DISK_IMG" bs=1M count=32 status=none
fi
qemu-system-i386 -cdrom "$ISO_FILE" -boot d \
    -drive file="$DISK_IMG",format=raw,index=0,media=disk \
    -display gtk,zoom-to-fit=on

echo -e "${YELLOW}QEMU exited${NC}"