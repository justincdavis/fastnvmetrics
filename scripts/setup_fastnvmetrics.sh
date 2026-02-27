#!/usr/bin/env bash
# setup_fastnvmetrics.sh — Grant user-level read access to Jetson debugfs
# metrics used by fastnvmetrics. Run once after each boot (requires sudo).
#
# Usage:
#   sudo bash scripts/setup_fastnvmetrics.sh
#
# What this does:
#   1. Makes /sys/kernel/debug traversable (o+rx)
#   2. Makes /sys/kernel/debug/cactmon/ traversable (o+rx)
#   3. Makes /sys/kernel/debug/cactmon/mc_all readable (o+r)
#   4. Makes /sys/kernel/debug/clk/emc/ traversable (o+rx) for EMC clock rate
#
# This allows fastnvmetrics to read EMC (memory controller) utilization
# without running the entire profiler as root.
#
# NOTE: These permissions reset on reboot. To persist, add this script
# to a systemd service or /etc/rc.local.

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: This script must be run as root (sudo)." >&2
    exit 1
fi

DEBUGFS="/sys/kernel/debug"
CACTMON="$DEBUGFS/cactmon"
MC_ALL="$CACTMON/mc_all"

echo "Setting up fastnvmetrics debugfs access..."

# Step 1: Make debugfs traversable
if [ -d "$DEBUGFS" ]; then
    chmod o+rx "$DEBUGFS"
    echo "  [OK] $DEBUGFS → o+rx"
else
    echo "  [SKIP] $DEBUGFS not found (debugfs not mounted?)"
    exit 1
fi

# Step 2: Make cactmon directory traversable
if [ -d "$CACTMON" ]; then
    chmod o+rx "$CACTMON"
    echo "  [OK] $CACTMON → o+rx"
else
    echo "  [SKIP] $CACTMON not found (tegra_cactmon_mc_all module not loaded?)"
    echo "  Try: modprobe tegra_cactmon_mc_all"
    exit 1
fi

# Step 3: Make mc_all readable
if [ -f "$MC_ALL" ]; then
    chmod o+r "$MC_ALL"
    echo "  [OK] $MC_ALL → o+r"
else
    echo "  [SKIP] $MC_ALL not found"
    exit 1
fi

# Step 4: Make clk/emc directory traversable (for EMC clock rate / DVFS)
CLK_DIR="$DEBUGFS/clk"
CLK_EMC="$CLK_DIR/emc"
if [ -d "$CLK_EMC" ]; then
    chmod o+rx "$CLK_DIR" "$CLK_EMC"
    echo "  [OK] $CLK_EMC → o+rx"
else
    echo "  [SKIP] $CLK_EMC not found (EMC DVFS tracking unavailable)"
fi

echo ""
echo "Done. EMC utilization is now readable by fastnvmetrics."
echo "Verify: cat $MC_ALL"
echo ""
echo "NOTE: These permissions reset on reboot."
echo "To persist, add to /etc/rc.local or create a systemd service."
