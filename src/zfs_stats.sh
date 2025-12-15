#!/bin/bash

# Function to read kstat value or return 0
get_val() {
    local file="/zfs-kstat/zfs/$1"
    if [ -f "$file" ]; then
        cat "$file"
    else
        echo 0
    fi
}

echo "=== ZFS-FUSE Statistics ==="
echo "Reading from /zfs-kstat..."

# --- Prefetch Stats ---
ZHITS=$(get_val "zfetchstats/hits")
ZMISS=$(get_val "zfetchstats/misses")
ZTOTAL=$((ZHITS + ZMISS))

if [ $ZTOTAL -gt 0 ]; then
    ZPERC=$(awk "BEGIN {printf \"%.2f\", 100 * $ZHITS / $ZTOTAL}")
else
    ZPERC="0.00"
fi

echo ""
echo "[Prefetch]"
echo "  Hits:   $ZHITS"
echo "  Misses: $ZMISS"
echo "  Total:  $ZTOTAL"
echo "  Eff:    $ZPERC%"

# --- ARC Stats ---
AHITS=$(get_val "arcstats/hits")
AMISS=$(get_val "arcstats/misses")
ATOTAL=$((AHITS + AMISS))

if [ $ATOTAL -gt 0 ]; then
    APERC=$(awk "BEGIN {printf \"%.2f\", 100 * $AHITS / $ATOTAL}")
else
    APERC="0.00"
fi

echo ""
echo "[ARC Cache]"
echo "  Hits:   $AHITS"
echo "  Misses: $AMISS"
echo "  Total:  $ATOTAL"
echo "  Eff:    $APERC%"
echo ""
