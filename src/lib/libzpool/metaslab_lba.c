/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2025 Antigravity Allocator.  All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/space_map.h>
#include <sys/metaslab.h>
#include <sys/metaslab_impl.h>
#include <sys/vdev_impl.h>
#include <sys/zio.h>
#include <sys/spa_impl.h>

#define	METASLAB_WEIGHT_PRIMARY		(1ULL << 63)
#define	METASLAB_WEIGHT_SECONDARY	(1ULL << 62)
#define	METASLAB_ACTIVE_MASK		\
	(METASLAB_WEIGHT_PRIMARY | METASLAB_WEIGHT_SECONDARY)

/*
 * Enable Debug Tracing for LBA
 * Uncomment to enable output to stderr (or change to zfs_dbgmsg for kernel/daemon logs)
 */
#define	LBA_DEBUG 1

#ifdef LBA_DEBUG
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

static void
lba_log_timestamp(void)
{
	static time_t last_sec = 0;
	struct timeval tv;
	struct tm tm_info;
	char buffer[32];
	
	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &tm_info);
	
	if (tv.tv_sec == last_sec) {
		/* Same second: only print msec, padded for alignment */
		/* Format: "hh.mm.ss.msec" -> 13 chars */
		/* We want to skip "hh.mm.ss" (8 chars) and align the .msec */
		fprintf(stderr, "        .%03ld ", tv.tv_usec / 1000);
	} else {
		/* New second: print full timestamp */
		strftime(buffer, sizeof(buffer), "%H.%M.%S", &tm_info);
		fprintf(stderr, "%s.%03ld ", buffer, tv.tv_usec / 1000);
		last_sec = tv.tv_sec;
	}
}

#define	LBA_TRACE(...)	do { \
	lba_log_timestamp(); \
	fprintf(stderr, "[LBA] " __VA_ARGS__); \
	fprintf(stderr, "\n"); \
} while (0)
#else
#define	LBA_TRACE(...)
#endif

/*
 * LBA (Locality Block Allocation) Strategy
 *
 * This allocator prioritizes data locality and separates metadata from data.
 * - Data is allocated starting from the beginning of the disk (low offsets).
 * - Metadata is allocated starting from the end of the disk (high offsets).
 * - Hints (e.g., from gang blocks or future CoW plumbing) are strictly respected for Vdev selection.
 */

#include <stdio.h> /* Ensure stdio is available */

/*
 * Helper to select a metaslab within a group based on the LBA strategy.
 */
static uint64_t
metaslab_lba_group_alloc(metaslab_group_t *mg, uint64_t psize, uint64_t txg,
    boolean_t is_metadata, dva_t *hintdva)
{
	vdev_t *vd = mg->mg_vd;
	int i;
	int start, end, step;
	uint64_t offset = -1ULL;

	ASSERT(vd->vdev_ms_count > 0);

	/*
	 * Direction depends on content type:
	 * Data: Start -> End (0 to count-1)
	 * Metadata: End -> Start (count-1 to 0)
	 */
	if (is_metadata) {
		start = vd->vdev_ms_count - 1;
		end = -1;
		step = -1;
	} else {
		start = 0;
		end = vd->vdev_ms_count;
		step = 1;
	}

	LBA_TRACE("Group Alloc: vdev=%llu type=%s start=%d end=%d", 
	    (u_longlong_t)vd->vdev_id, is_metadata ? "META" : "DATA", start, end);

	for (i = start; i != end; i += step) {
		metaslab_t *msp = vd->vdev_ms[i];
		
		/*
		 * Quick check: Does it have enough space?
		 * We check deferspace (space freed in this txg) + alloc matches size?
		 * Actually, we check if the map has enough free space.
		 * msp->ms_map.sm_space is the free space, but it might not be loaded.
		 * We use the weight (which roughly tracks free space) or try to load.
		 */
		
		mutex_enter(&msp->ms_lock);

		/*
		 * If not loaded, and heavily allocated, maybe skip?
		 * LBA prefers strict layout, so we try anyway unless it is completely full.
		 */
		if (msp->ms_map.sm_space < psize && msp->ms_map.sm_loaded) {
			mutex_exit(&msp->ms_lock);
			continue;
		}

		/*
		 * Activate the metaslab (load space map if needed).
		 * We use METASLAB_WEIGHT_PRIMARY to indicate we really want this one?
		 * The weight logic in activate is complex, but we just want to ensure it is loaded.
		 */
		if (metaslab_activate(msp, METASLAB_WEIGHT_PRIMARY, psize) != 0) {
			mutex_exit(&msp->ms_lock);
			continue;
		}

		/*
		 * Try to allocate.
		 */
		offset = space_map_alloc(&msp->ms_map, psize);

		if (offset != -1ULL) {
			/*
			 * Found a block!
			 * Update allocations.
			 */
			if (spa_writeable(mg->mg_class->mc_spa)) {
				space_map_add(&msp->ms_allocmap[txg & TXG_MASK], offset, psize);
				vdev_dirty(vd, VDD_METASLAB, msp, txg);
			}
			msp->ms_weight &= ~METASLAB_WEIGHT_PRIMARY; /* clear active flag? logic from legacy */
			mutex_exit(&msp->ms_lock);
			LBA_TRACE("Allocated: ms=%d offset=%llu", i, (u_longlong_t)offset);
			return (offset);
		}
		
		mutex_exit(&msp->ms_lock);
	}

	LBA_TRACE("Failed to allocate in group vdev=%llu", (u_longlong_t)vd->vdev_id);
	return (-1ULL);
}

int
metaslab_lba_alloc(spa_t *spa, metaslab_class_t *mc, uint64_t psize,
    blkptr_t *bp, int ndvas, uint64_t txg, blkptr_t *hintbp, int flags,
    metaslab_alloc_ctx_t *ctx)
{
	dva_t *dva = bp->blk_dva;
	dva_t *hintdva = hintbp ? hintbp->blk_dva : NULL;
	int error = 0;
	int d;
	boolean_t is_metadata = dmu_ot[ctx->mac_obj_type].ot_metadata;

	LBA_TRACE("metaslab_lba_alloc called");

	ASSERT(bp->blk_birth == 0);
	ASSERT(BP_PHYSICAL_BIRTH(bp) == 0);

	LBA_TRACE("Alloc Start: size=%llu type=%d (%s) ndvas=%d hint=%p", 
	    (u_longlong_t)psize, ctx->mac_obj_type, is_metadata ? "META" : "DATA", ndvas, hintbp);

	spa_config_enter(spa, SCL_ALLOC, FTAG, RW_READER);

	if (mc->mc_rotor == NULL) {
		spa_config_exit(spa, SCL_ALLOC, FTAG);
		return (ENOSPC);
	}

	for (d = 0; d < ndvas; d++) {
		vdev_t *vd = NULL;
		metaslab_group_t *mg = NULL;
		uint64_t offset = -1ULL;
		uint64_t asize = 0;

		/*
		 * 1. Target Vdev Selection
		 */
		if (hintdva && d < BP_GET_NDVAS(hintbp)) {
			// Respect hint
			vd = vdev_lookup_top(spa, DVA_GET_VDEV(&hintdva[d]));
			if (vd && vd->vdev_mg) mg = vd->vdev_mg;
			LBA_TRACE("Hint Used: dva=%d vdev=%llu", d, (u_longlong_t)(vd ? vd->vdev_id : -1));
		} else if (d == 0) {
			// First copy: Use rotor
			mg = mc->mc_rotor;
			vd = mg->mg_vd;
			LBA_TRACE("Rotor Used: vdev=%llu", (u_longlong_t)vd->vdev_id);
		} else {
			// Simultaneous copies: Prefer distinct vdevs
			// Simple fallback: use rotor->next
			// (A real implementation would be smarter here)
			metaslab_group_t *rot = mc->mc_rotor;
			for (int k = 0; k < d; k++) rot = rot->mg_next;
			mg = rot;
			vd = mg->mg_vd;
			LBA_TRACE("Mirror/Copy Used: vdev=%llu", (u_longlong_t)vd->vdev_id);
		}

		if (!mg) {
			error = ENOSPC;
			goto fail;
		}
		
		/*
		 * 2. Scan for space in the selected group (and others if needed)
		 * We iterate through groups starting from the chosen one.
		 */
		metaslab_group_t *start_mg = mg;
		do {
			if (mg->mg_activation_count <= 0) /* Skip inactive/removed */
				goto next_group;

			asize = vdev_psize_to_asize(mg->mg_vd, psize);
			
			offset = metaslab_lba_group_alloc(mg, asize, txg, is_metadata, hintdva);
			
			if (offset != -1ULL) {
				// Success!
				DVA_SET_VDEV(&dva[d], mg->mg_vd->vdev_id);
				DVA_SET_OFFSET(&dva[d], offset);
				DVA_SET_GANG(&dva[d], !!(flags & METASLAB_GANG_HEADER));
				DVA_SET_ASIZE(&dva[d], asize);
				goto next_dva;
			}

next_group:
			mg = mg->mg_next;
		} while (mg != start_mg);

		// If we are here, we failed to find space for this DVA
		error = ENOSPC;
		goto fail;

next_dva:
		continue;
	}

	spa_config_exit(spa, SCL_ALLOC, FTAG);
	BP_SET_BIRTH(bp, txg, txg);
	return (0);

fail:
	// Rollback
	for (int i = 0; i < d; i++) {
		metaslab_free_dva(spa, &dva[i], txg, B_TRUE);
		bzero(&dva[i], sizeof (dva_t));
	}
	spa_config_exit(spa, SCL_ALLOC, FTAG);
	LBA_TRACE("Alloc Failed: error=%d", error);
	return (error);
}

void
metaslab_lba_free(spa_t *spa, const blkptr_t *bp, uint64_t txg, boolean_t now)
{
	metaslab_legacy_free(spa, bp, txg, now);
}

int
metaslab_lba_claim(spa_t *spa, const blkptr_t *bp, uint64_t txg)
{
	return (metaslab_legacy_claim(spa, bp, txg));
}

metaslab_ops_t metaslab_ops_lba = {
	"lba",
	metaslab_lba_alloc,
	metaslab_lba_free,
	metaslab_lba_claim,
	NULL,
	NULL
};
