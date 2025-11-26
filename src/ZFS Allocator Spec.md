# ZFS Allocator Spec

## Overview

This document analyzes the current implementation of the ZFS allocator, specifically focusing on the `metaslab.c` file and the allocation hint mechanism. The goal is to understand how the allocator selects blocks and where the current "hint" mechanism is utilized, to identify opportunities for improving disk allocation locality for metadata-heavy workloads.

## Allocation Flow

The allocation process begins in `metaslab_alloc` and proceeds as follows:

1.  **`metaslab_alloc`**: The entry point for allocation. It iterates over the required number of DVAs (copies) and calls `metaslab_alloc_dva` for each.
2.  **`metaslab_alloc_dva`**: Responsible for selecting a Top-Level Vdev (device) and a Metaslab Group.
3.  **`metaslab_group_alloc`**: Responsible for selecting a specific Metaslab within the chosen group.
4.  **`space_map_alloc`**: Responsible for selecting a specific block offset within the chosen Metaslab.

## The Role of Allocation Hints

The `metaslab_alloc` function accepts a `hintbp` (hint block pointer), which contains `hintdva` (hint Data Virtual Address).

### 1. Vdev Selection (Inter-Device Locality)

The `hintdva` is primarily used in `metaslab_alloc_dva` to select the **Vdev**.

```c
// lib/libzpool/metaslab.c

if (hintdva) {
    vd = vdev_lookup_top(spa, DVA_GET_VDEV(&hintdva[d]));
    // ...
    mg = vd->vdev_mg;
    // ...
}
```

*   If a hint is provided, the allocator attempts to start allocation on the Vdev specified by the hint.
*   This ensures **Inter-Device Locality**: related blocks (e.g., metadata and data, or gang blocks) try to stay on the same disk.

### 2. Metaslab Selection (Intra-Device Locality)

Once a Vdev (and its corresponding Metaslab Group `mg`) is selected, `metaslab_alloc_dva` iterates through the Metaslab Groups (if the first one fails) and calls `metaslab_group_alloc`.

**Crucially, the `hintdva` offset is NOT passed to `metaslab_group_alloc`.**

`metaslab_group_alloc` selects a metaslab based on **Weight**. It iterates through the metaslabs in the group (sorted by weight) and picks the first one that satisfies the size and distance requirements.

#### Metaslab Weighting (`metaslab_weight`)

The `metaslab_weight` function determines the priority of metaslabs. The weight is calculated based on:

1.  **Free Space**: Baseline weight is the amount of free space.
2.  **Bandwidth (Offset)**: Lower offsets (outer tracks) get a multiplier (up to 2x).
    ```c
    weight = 2 * weight - ((sm->sm_start >> vd->vdev_ms_shift) * weight) / vd->vdev_ms_count;
    ```
3.  **Bonus Area**: Metaslabs that have been previously activated (below `mg_bonus_area`) get a bonus (150%).
    ```c
    if (sm->sm_start <= mg->mg_bonus_area)
        weight *= (metaslab_smo_bonus_pct / 100);
    ```
4.  **Active Status**: Currently active metaslabs are preferred.

**Observation**: The weighting algorithm does **not** consider proximity to the `hintdva` offset. It favors free space and the beginning of the disk. This means even if the hint points to a specific region, the allocator may choose a completely different metaslab if it has a higher weight.

#### Distance Check

`metaslab_group_alloc` performs a distance check using `metaslab_distance`.

```c
target_distance = min_distance + (msp->ms_smo.smo_alloc ? 0 : min_distance >> 1);

for (i = 0; i < d; i++)
    if (metaslab_distance(msp, &dva[i]) < target_distance)
        break;
```

*   This checks the distance between the candidate metaslab and **other copies** (`dva[0]` to `dva[d-1]`) of the *same block* being allocated.
*   This is for **Redundancy/Diversity**, ensuring copies are spread out.
*   It does **not** check distance from the `hintdva`.

### 3. Block Selection (Intra-Metaslab Locality)

Once a metaslab is selected, `space_map_alloc` is called. This delegates to specific allocator implementations (e.g., `metaslab_df_alloc`).

*   `metaslab_df_alloc` uses `metaslab_block_picker`.
*   `metaslab_block_picker` uses a `cursor` (`sm->sm_pp_cursor`) to find the next free block.
*   The `cursor` tracks the *last allocation* in that metaslab.

**Observation**: The hint offset is not passed to `space_map_alloc`. The allocator tries to be contiguous with the *previous allocation* in that metaslab, but it is unaware of the *current hint*.

## Summary of Findings

1.  **Vdev Locality**: The hint **is used** to select the correct Vdev.
2.  **Metaslab Locality**: The hint **is NOT used** to select the metaslab. Metaslab selection is driven by free space and global disk position (outer tracks), not proximity to the hint.
3.  **Block Locality**: The hint **is NOT used** to select the block within the metaslab.


## Hint Usage Analysis

The user asked: *"What do hintdva values mean actually? Does it point to the block CoW will overwrite?"*

Based on the code analysis of `zio.c` and `metaslab.c`, here is the breakdown:

### 1. Standard CoW Writes (Data & Metadata)
**Answer: NO.**

In the standard write path (`zio_dva_allocate`), the `hintbp` passed to `metaslab_alloc` is explicitly `NULL`.

```c
// lib/libzpool/zio.c : zio_dva_allocate
error = metaslab_alloc(spa, mc, zio->io_size, bp,
    zio->io_prop.zp_copies, zio->io_txg, NULL, 0);
```

*   The allocator has **no knowledge** of the "old" block location during a Copy-on-Write operation.
*   It does **not** try to allocate the new block near the old block.
*   It relies entirely on the rotor (round-robin across Vdevs) and Metaslab Weight (free space/offset) for placement.

### 2. Gang Blocks
**Answer: Points to Gang Header.**

When allocating "Gang Members" (fragments of a large block that didn't fit contiguously), the `hintbp` points to the **Gang Header**.

```c
// lib/libzpool/zio.c : zio_write_gang_block
error = metaslab_alloc(..., pio == gio ? NULL : gio->io_bp,
    METASLAB_HINTBP_FAVOR | METASLAB_GANG_HEADER);
```

*   **Purpose**: To keep gang members on the same Vdev as the header (if possible), or at least near it.
*   **Behavior**: Uses `METASLAB_HINTBP_FAVOR`.

### 3. ZIL (Intent Log) Blocks
**Answer: Points to Previous Log Block.**

When allocating blocks for the ZIL, the `hintbp` points to the **previous block** in the log chain.

```c
// lib/libzpool/zio.c : zio_alloc_zil
error = metaslab_alloc(..., old_bp, METASLAB_HINTBP_AVOID);
```

*   **Purpose**: To avoid allocating the new log block on the same metaslab as the previous one (likely for performance/parallelism or wear leveling).
*   **Behavior**: Uses `METASLAB_HINTBP_AVOID`.


### 4. Ditto Blocks (Copies > 1)
**Answer: Previous Copy acts as Hint.**

For blocks with multiple copies (`copies=2` or `copies=3`), the allocator uses the location of the *previous copy* as a hint for the *next copy*.

```c
// lib/libzpool/metaslab.c : metaslab_alloc_dva
} else if (d != 0) {
    vd = vdev_lookup_top(spa, DVA_GET_VDEV(&dva[d - 1]));
    mg = vd->vdev_mg->mg_next;
}
```

*   **Purpose**: To force the next copy to be on a **different Vdev** (specifically, the next one in the rotor).
*   **Behavior**: This is hardcoded logic for `d > 0`, ensuring redundancy.

## Key Takeaways

1.  **Hint Usage is Mixed (Favor vs. Avoid)**:
    *   **Gang Blocks**: Use hints to **Favor** the same Vdev (keep fragments together).
    *   **ZIL & Ditto Blocks**: Use hints to **Avoid** the same Vdev (spread load or ensure redundancy).
    *   **Standard Writes**: Do **not** use hints.

2.  **Gang Blocks are for Fragmentation/Fullness**:
    *   Gang blocks are only used as a fallback when a contiguous allocation of the requested size fails (`ENOSPC`).
    *   This typically happens when the pool is very full or highly fragmented.

## Conclusion

The current implementation **does not** use the "Old Block" as a hint for CoW updates. This confirms that simply enabling hints won't suffice; we would need to:
1.  Plumb the "Old BP" through the `zio_write` stack down to `zio_dva_allocate`.
2.  Pass it as `hintbp` to `metaslab_alloc`.
3.  Update `metaslab_alloc` logic to actually *use* this hint for Metaslab selection (as noted in the previous section).

## Proposed Architecture: Pluggable Allocator

To support alternative allocation strategies (like "Locality Block Allocation" - LBA) alongside the existing implementation, we propose refactoring the allocator to use a pluggable interface.

### 1. Public Interface

The public interface of the allocator is defined in `lib/libzfscommon/include/sys/metaslab.h`. The core functions that define an allocation strategy are:

```c
// Core Allocation Logic
int metaslab_alloc(spa_t *spa, metaslab_class_t *mc, uint64_t psize,
    blkptr_t *bp, int ncopies, uint64_t txg, blkptr_t *hintbp, int flags);

void metaslab_free(spa_t *spa, const blkptr_t *bp, uint64_t txg, boolean_t now);

int metaslab_claim(spa_t *spa, const blkptr_t *bp, uint64_t txg);

// Sync & Lifecycle (May need to be pluggable if strategy affects on-disk format)
void metaslab_sync(metaslab_t *msp, uint64_t txg);
void metaslab_sync_done(metaslab_t *msp, uint64_t txg);
```


### 2. Proposed Abstraction (`metaslab_ops_t`)

We will introduce a `metaslab_alloc_ctx_t` structure to capture the context of the allocation request, and update the `metaslab_ops_t` to use it.

```c
typedef struct metaslab_alloc_ctx {
    // User Space Context
    uint64_t mac_uid;       // User ID
    uint64_t mac_gid;       // Group ID
    uint64_t mac_pid;       // Process ID
    uint64_t mac_pgid;      // Process Group ID

    // I/O Context
    dmu_object_type_t mac_obj_type; // ZIO Object Type (e.g., DMU_OT_PLAIN_FILE_CONTENTS)
} metaslab_alloc_ctx_t;

typedef struct metaslab_ops {
    const char *msop_name;
    
    // Allocation
    int (*msop_alloc)(spa_t *spa, metaslab_class_t *mc, uint64_t psize,
        blkptr_t *bp, int ncopies, uint64_t txg, blkptr_t *hintbp, int flags,
        metaslab_alloc_ctx_t *ctx);
        
    // Frees & Claims
    void (*msop_free)(spa_t *spa, const blkptr_t *bp, uint64_t txg, boolean_t now);
    int (*msop_claim)(spa_t *spa, const blkptr_t *bp, uint64_t txg);
    
    // Syncing (Optional: if strategy requires custom sync logic)
    void (*msop_sync)(metaslab_t *msp, uint64_t txg);
    void (*msop_sync_done)(metaslab_t *msp, uint64_t txg);
} metaslab_ops_t;
```

### 3. Implementation Plan

1.  **Refactor `metaslab.c`**:
    *   Rename existing functions to `metaslab_legacy_alloc`, `metaslab_legacy_free`, etc.
    *   Update `metaslab_alloc` to accept `metaslab_alloc_ctx_t *ctx`.
    *   Implement the global `metaslab_alloc` as a dispatcher that checks the `alloc_strategy` property and calls the appropriate `msop_alloc`.
2.  **Update Callers**:
    *   Update `zio_dva_allocate` and other callers to populate and pass `metaslab_alloc_ctx_t`.
3.  **Create `metaslab_lba.c`**:
    *   Implement `metaslab_lba_alloc`, `metaslab_lba_free`, etc.
    *   This implementation will focus on **Locality**:
        *   Respecting `hintbp` for Metaslab selection.
        *   Using `hintbp` offset for Block selection.
        *   Using `ctx->mac_obj_type` to potentially treat metadata differently.
4.  **Configuration**:
    *   Add a ZFS property `alloc_strategy` (enum: `legacy`, `lba`).
