#!/bin/bash

# measure_hdd.sh - Measure HDD I/O stats from /proc/diskstats
# Usage:
#   ./measure_hdd.sh <disk> init
#   ./measure_hdd.sh <disk> show [interval_sec]
#   ./measure_hdd.sh <disk> done

set -euo pipefail

DISK="$1"
MODE="$2"
SLEEP_SEC="${3:-}"

if [[ -z "${DISK:-}" || -z "${MODE:-}" ]]; then
    echo "Usage:" >&2
    echo "  $0 <disk> init" >&2
    echo "  $0 <disk> show [interval_sec]" >&2
    echo "  $0 <disk> done" >&2
    exit 1
fi

# Validate disk name
if [[ ! "$DISK" =~ ^[a-zA-Z0-9._-]+$ ]]; then
    echo "Error: Invalid disk name '$DISK'" >&2
    exit 1
fi

STAT_FILE="/tmp/${DISK}.stats"

# Helper: extract fields 4-14 for exact disk match
get_diskstats() {
    local d=$1
    awk -v disk="$d" '$3 == disk {
        print $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14
        exit
    }' /proc/diskstats
}

# Ensure disk exists
if ! get_diskstats "$DISK" >/dev/null; then
    echo "Error: Disk '$DISK' not found in /proc/diskstats" >&2
    exit 1
fi

case "$MODE" in
    init)
        STATS=$(get_diskstats "$DISK")
        if [[ -z "$STATS" ]]; then
            echo "Error: Could not read stats for '$DISK'" >&2
            exit 1
        fi
        echo "$STATS" > "$STAT_FILE"
        echo "Initial stats saved to $STAT_FILE"
        ;;

    show)
        if [[ ! -f "$STAT_FILE" ]]; then
            echo "Error: No initial stats. Run 'init' first." >&2
            exit 1
        fi

        calc_and_print_delta() {
            # Read baseline
            IFS=' ' read -r r_comp1 r_merg1 s_read1 r_time1 \
                        w_comp1 w_merg1 s_writ1 w_time1 \
                        io_prog1 io_time1 w_io_time1 < "$STAT_FILE"

            # Read current
            CURRENT=$(get_diskstats "$DISK")
            [[ -z "$CURRENT" ]] && { echo "Warning: Failed to read current stats" >&2; return; }
            IFS=' ' read -r r_comp2 r_merg2 s_read2 r_time2 \
                        w_comp2 w_merg2 s_writ2 w_time2 \
                        io_prog2 io_time2 w_io_time2 <<< "$CURRENT"

            # Deltas
            delta_reads=$(( r_comp2 - r_comp1 ))
            delta_writes=$(( w_comp2 - w_comp1 ))
            delta_read_time=$(( r_time2 - r_time1 ))
            delta_write_time=$(( w_time2 - w_time1 ))
            delta_sectors_read=$(( s_read2 - s_read1 ))
            delta_sectors_written=$(( s_writ2 - s_writ1 ))

            # Clamp negatives to zero
            (( delta_reads < 0 )) && delta_reads=0
            (( delta_writes < 0 )) && delta_writes=0
            (( delta_read_time < 0 )) && delta_read_time=0
            (( delta_write_time < 0 )) && delta_write_time=0
            (( delta_sectors_read < 0 )) && delta_sectors_read=0
            (( delta_sectors_written < 0 )) && delta_sectors_written=0

            avg_read_latency=0
            avg_write_latency=0
            (( delta_reads > 0 )) && avg_read_latency=$(( delta_read_time / delta_reads ))
            (( delta_writes > 0 )) && avg_write_latency=$(( delta_write_time / delta_writes ))

            # Output
            printf "%-20s %s\n" "Disk:" "$DISK"
            printf "%-20s %'d\n" "Reads completed:" "$delta_reads"
            printf "%-20s %'d\n" "Writes completed:" "$delta_writes"
            printf "%-20s %'d (≈ %'.1f MiB)\n" "Sectors read:" "$delta_sectors_read" $((delta_sectors_read * 512 / 1024 / 1024))
            printf "%-20s %'d (≈ %'.1f MiB)\n" "Sectors written:" "$delta_sectors_written" $((delta_sectors_written * 512 / 1024 / 1024))
            printf "%-20s %'d ms\n" "Total read time:" "$delta_read_time"
            printf "%-20s %'d ms\n" "Total write time:" "$delta_write_time"
            printf "%-20s %'d ms\n" "Avg read latency:" "$avg_read_latency"
            printf "%-20s %'d ms\n" "Avg write latency:" "$avg_write_latency"
            echo "----------------------------------------"
        }

        if [[ -z "$SLEEP_SEC" ]]; then
            # One-shot mode
            calc_and_print_delta
        else
            # Validate sleep interval
            if ! [[ "$SLEEP_SEC" =~ ^[0-9]+$ ]] || [[ "$SLEEP_SEC" -eq 0 ]]; then
                echo "Error: sleep_seconds must be a positive integer" >&2
                exit 1
            fi

            echo "Monitoring '$DISK' every ${SLEEP_SEC} seconds. Press Ctrl+C to stop."
            echo "----------------------------------------"
            while true; do
                calc_and_print_delta
                sleep "$SLEEP_SEC"
            done
        fi
        ;;

    done)
        if [[ -n "$SLEEP_SEC" ]]; then
            echo "Error: 'done' mode does not accept sleep_seconds" >&2
            exit 1
        fi

        if [[ ! -f "$STAT_FILE" ]]; then
            echo "Error: No initial stats file found" >&2
            exit 1
        fi

        # Temporarily act like 'show' but without loop
        SLEEP_SEC=""  # Force one-shot
        # Reuse logic — but can't call function before definition, so duplicate minimal logic
        IFS=' ' read -r r_comp1 r_merg1 s_read1 r_time1 \
                    w_comp1 w_merg1 s_writ1 w_time1 \
                    io_prog1 io_time1 w_io_time1 < "$STAT_FILE"

        CURRENT=$(get_diskstats "$DISK")
        [[ -z "$CURRENT" ]] && { echo "Error: Failed to read current stats" >&2; exit 1; }
        IFS=' ' read -r r_comp2 r_merg2 s_read2 r_time2 \
                    w_comp2 w_merg2 s_writ2 w_time2 \
                    io_prog2 io_time2 w_io_time2 <<< "$CURRENT"

        delta_reads=$(( r_comp2 - r_comp1 ))
        delta_writes=$(( w_comp2 - w_comp1 ))
        delta_read_time=$(( r_time2 - r_time1 ))
        delta_write_time=$(( w_time2 - w_time1 ))
        delta_sectors_read=$(( s_read2 - s_read1 ))
        delta_sectors_written=$(( s_writ2 - s_writ1 ))

        (( delta_reads < 0 )) && delta_reads=0
        (( delta_writes < 0 )) && delta_writes=0
        (( delta_read_time < 0 )) && delta_read_time=0
        (( delta_write_time < 0 )) && delta_write_time=0
        (( delta_sectors_read < 0 )) && delta_sectors_read=0
        (( delta_sectors_written < 0 )) && delta_sectors_written=0

        avg_read_latency=0
        avg_write_latency=0
        (( delta_reads > 0 )) && avg_read_latency=$(( delta_read_time / delta_reads ))
        (( delta_writes > 0 )) && avg_write_latency=$(( delta_write_time / delta_writes ))

        echo "=== FINAL I/O STATISTICS FOR $DISK ==="
        printf "%-20s %'d\n" "Reads completed:" "$delta_reads"
        printf "%-20s %'d\n" "Writes completed:" "$delta_writes"
        printf "%-20s %'d (≈ %'.1f MiB)\n" "Sectors read:" "$delta_sectors_read" $((delta_sectors_read * 512 / 1024 / 1024))
        printf "%-20s %'d (≈ %'.1f MiB)\n" "Sectors written:" "$delta_sectors_written" $((delta_sectors_written * 512 / 1024 / 1024))
        printf "%-20s %'d ms\n" "Total read time:" "$delta_read_time"
        printf "%-20s %'d ms\n" "Total write time:" "$delta_write_time"
        printf "%-20s %'d ms\n" "Avg read latency:" "$avg_read_latency"
        printf "%-20s %'d ms\n" "Avg write latency:" "$avg_write_latency"

        rm -f "$STAT_FILE"
        echo "Stats file removed."
        ;;

    *)
        echo "Error: Invalid mode '$MODE'. Use 'init', 'show', or 'done'." >&2
        exit 1
        ;;
esac