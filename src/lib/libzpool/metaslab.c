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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/zfs_context.h>
#include <sys/dmu.h>
#include <sys/dmu_tx.h>
#include <sys/space_map.h>
#include <sys/metaslab_impl.h>
#include <sys/vdev_impl.h>
#include <sys/zio.h>
#include <sys/alloc_bias_backend.h>

#include <sys/dmu_objset.h>
#include <time.h>
#include <inttypes.h>

uint64_t metaslab_aliquot = 512ULL << 10;
uint64_t metaslab_gang_bang = SPA_MAXBLOCKSIZE + 1;	/* force gang blocks */

#define ALLOC_DEBUG

/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:        the pointer to the member.
 * @type:       the type of the container struct this is embedded in.
 * @member:     the name of the member within the struct.
 */
#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

/*
 * Metaslab debugging: when set, keeps all space maps in core to verify frees.
 */
static int metaslab_debug = 0;

/*
 * Minimum size which forces the dynamic allocator to change
 * it's allocation strategy.  Once the space map cannot satisfy
 * an allocation of this size then it switches to using more
 * aggressive strategy (i.e search by size rather than offset).
 */
uint64_t metaslab_df_alloc_threshold = SPA_MAXBLOCKSIZE;

/*
 * The minimum free space, in percent, which must be available
 * in a space map to continue allocations in a first-fit fashion.
 * Once the space_map's free space drops below this level we dynamically
 * switch to using best-fit allocations.
 */
int metaslab_df_free_pct = 4;

/*
 * A metaslab is considered "free" if it contains a contiguous
 * segment which is greater than metaslab_min_alloc_size.
 */
uint64_t metaslab_min_alloc_size = DMU_MAX_ACCESS;

/*
 * Max number of space_maps to prefetch.
 */
int metaslab_prefetch_limit = SPA_DVAS_PER_BP;

/*
 * Percentage bonus multiplier for metaslabs that are in the bonus area.
 */
int metaslab_smo_bonus_pct = 150;

/*
 * ==========================================================================
 * Metaslab classes
 * ==========================================================================
 */
metaslab_class_t *
metaslab_class_create(spa_t *spa, space_map_ops_t *ops)
{
	metaslab_class_t *mc;

	mc = kmem_zalloc(sizeof (metaslab_class_t), KM_SLEEP);

	mc->mc_spa = spa;
	mc->mc_rotor = NULL;
	mc->mc_ops = ops;

	return (mc);
}

void
metaslab_class_destroy(metaslab_class_t *mc)
{
	ASSERT(mc->mc_rotor == NULL);
	ASSERT(mc->mc_alloc == 0);
	ASSERT(mc->mc_deferred == 0);
	ASSERT(mc->mc_space == 0);
	ASSERT(mc->mc_dspace == 0);

	kmem_free(mc, sizeof (metaslab_class_t));
}

int
metaslab_class_validate(metaslab_class_t *mc)
{
	metaslab_group_t *mg;
	vdev_t *vd;

	/*
	 * Must hold one of the spa_config locks.
	 */
	ASSERT(spa_config_held(mc->mc_spa, SCL_ALL, RW_READER) ||
	    spa_config_held(mc->mc_spa, SCL_ALL, RW_WRITER));

	if ((mg = mc->mc_rotor) == NULL)
		return (0);

	do {
		vd = mg->mg_vd;
		ASSERT(vd->vdev_mg != NULL);
		ASSERT3P(vd->vdev_top, ==, vd);
		ASSERT3P(mg->mg_class, ==, mc);
		ASSERT3P(vd->vdev_ops, !=, &vdev_hole_ops);
	} while ((mg = mg->mg_next) != mc->mc_rotor);

	return (0);
}

void
metaslab_class_space_update(metaslab_class_t *mc, int64_t alloc_delta,
    int64_t defer_delta, int64_t space_delta, int64_t dspace_delta)
{
	atomic_add_64(&mc->mc_alloc, alloc_delta);
	atomic_add_64(&mc->mc_deferred, defer_delta);
	atomic_add_64(&mc->mc_space, space_delta);
	atomic_add_64(&mc->mc_dspace, dspace_delta);
}

uint64_t
metaslab_class_get_alloc(metaslab_class_t *mc)
{
	return (mc->mc_alloc);
}

uint64_t
metaslab_class_get_deferred(metaslab_class_t *mc)
{
	return (mc->mc_deferred);
}

uint64_t
metaslab_class_get_space(metaslab_class_t *mc)
{
	return (mc->mc_space);
}

uint64_t
metaslab_class_get_dspace(metaslab_class_t *mc)
{
	return (spa_deflate(mc->mc_spa) ? mc->mc_dspace : mc->mc_space);
}

/*
 * ==========================================================================
 * Metaslab groups
 * ==========================================================================
 */
static int
metaslab_compare(const void *x1, const void *x2)
{
	const metaslab_t *m1 = x1;
	const metaslab_t *m2 = x2;

	if (m1->ms_weight < m2->ms_weight)
		return (1);
	if (m1->ms_weight > m2->ms_weight)
		return (-1);

	/*
	 * If the weights are identical, use the offset to force uniqueness.
	 */
	if (m1->ms_map.sm_start < m2->ms_map.sm_start)
		return (-1);
	if (m1->ms_map.sm_start > m2->ms_map.sm_start)
		return (1);

	ASSERT3P(m1, ==, m2);

	return (0);
}

metaslab_group_t *
metaslab_group_create(metaslab_class_t *mc, vdev_t *vd)
{
	metaslab_group_t *mg;

	mg = kmem_zalloc(sizeof (metaslab_group_t), KM_SLEEP);
	mutex_init(&mg->mg_lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&mg->mg_metaslab_tree, metaslab_compare,
	    sizeof (metaslab_t), offsetof(struct metaslab, ms_group_node));
	mg->mg_vd = vd;
	mg->mg_class = mc;
	mg->mg_activation_count = 0;

	return (mg);
}

void
metaslab_group_destroy(metaslab_group_t *mg)
{
	ASSERT(mg->mg_prev == NULL);
	ASSERT(mg->mg_next == NULL);
	/*
	 * We may have gone below zero with the activation count
	 * either because we never activated in the first place or
	 * because we're done, and possibly removing the vdev.
	 */
	ASSERT(mg->mg_activation_count <= 0);

	avl_destroy(&mg->mg_metaslab_tree);
	mutex_destroy(&mg->mg_lock);
	kmem_free(mg, sizeof (metaslab_group_t));
}

void
metaslab_group_activate(metaslab_group_t *mg)
{
	metaslab_class_t *mc = mg->mg_class;
	metaslab_group_t *mgprev, *mgnext;

	ASSERT(spa_config_held(mc->mc_spa, SCL_ALLOC, RW_WRITER));

	ASSERT(mc->mc_rotor != mg);
	ASSERT(mg->mg_prev == NULL);
	ASSERT(mg->mg_next == NULL);
	ASSERT(mg->mg_activation_count <= 0);

	if (++mg->mg_activation_count <= 0)
		return;

	mg->mg_aliquot = metaslab_aliquot * MAX(1, mg->mg_vd->vdev_children);

	if ((mgprev = mc->mc_rotor) == NULL) {
		mg->mg_prev = mg;
		mg->mg_next = mg;
	} else {
		mgnext = mgprev->mg_next;
		mg->mg_prev = mgprev;
		mg->mg_next = mgnext;
		mgprev->mg_next = mg;
		mgnext->mg_prev = mg;
	}
	mc->mc_rotor = mg;
}

void
metaslab_group_passivate(metaslab_group_t *mg)
{
	metaslab_class_t *mc = mg->mg_class;
	metaslab_group_t *mgprev, *mgnext;

	ASSERT(spa_config_held(mc->mc_spa, SCL_ALLOC, RW_WRITER));

	if (--mg->mg_activation_count != 0) {
		ASSERT(mc->mc_rotor != mg);
		ASSERT(mg->mg_prev == NULL);
		ASSERT(mg->mg_next == NULL);
		ASSERT(mg->mg_activation_count < 0);
		return;
	}

	mgprev = mg->mg_prev;
	mgnext = mg->mg_next;

	if (mg == mgnext) {
		mc->mc_rotor = NULL;
	} else {
		mc->mc_rotor = mgnext;
		mgprev->mg_next = mgnext;
		mgnext->mg_prev = mgprev;
	}

	mg->mg_prev = NULL;
	mg->mg_next = NULL;
}

static void
metaslab_group_add(metaslab_group_t *mg, metaslab_t *msp)
{
	mutex_enter(&mg->mg_lock);
	ASSERT(msp->ms_group == NULL);
	msp->ms_group = mg;
	msp->ms_weight = 0;
	avl_add(&mg->mg_metaslab_tree, msp);
	mutex_exit(&mg->mg_lock);
}

static void
metaslab_group_remove(metaslab_group_t *mg, metaslab_t *msp)
{
	mutex_enter(&mg->mg_lock);
	ASSERT(msp->ms_group == mg);
	avl_remove(&mg->mg_metaslab_tree, msp);
	msp->ms_group = NULL;
	mutex_exit(&mg->mg_lock);
}

static void
metaslab_group_sort(metaslab_group_t *mg, metaslab_t *msp, uint64_t weight)
{
	/*
	 * Although in principle the weight can be any value, in
	 * practice we do not use values in the range [1, 510].
	 */
	ASSERT(weight >= SPA_MINBLOCKSIZE-1 || weight == 0);
	ASSERT(MUTEX_HELD(&msp->ms_lock));

	mutex_enter(&mg->mg_lock);
	ASSERT(msp->ms_group == mg);
	avl_remove(&mg->mg_metaslab_tree, msp);
	msp->ms_weight = weight;
	avl_add(&mg->mg_metaslab_tree, msp);
	mutex_exit(&mg->mg_lock);
}

/*
 * ==========================================================================
 * Common allocator routines
 * ==========================================================================
 */
static int
metaslab_segsize_compare(const void *x1, const void *x2)
{
	const space_seg_t *s1 = x1;
	const space_seg_t *s2 = x2;
	uint64_t ss_size1 = s1->ss_end - s1->ss_start;
	uint64_t ss_size2 = s2->ss_end - s2->ss_start;

	if (ss_size1 < ss_size2)
		return (-1);
	if (ss_size1 > ss_size2)
		return (1);

	if (s1->ss_start < s2->ss_start)
		return (-1);
	if (s1->ss_start > s2->ss_start)
		return (1);

	return (0);
}

/*
 * This is a helper function that can be used by the allocator to find
 * a suitable block to allocate. This will search the specified AVL
 * tree looking for a block that matches the specified criteria.
 */
static uint64_t
metaslab_block_picker(avl_tree_t *t, uint64_t *cursor, uint64_t size,
    uint64_t align)
{
	space_seg_t *ss, ssearch;
	avl_index_t where;

	ssearch.ss_start = *cursor;
	ssearch.ss_end = *cursor + size;

	ss = avl_find(t, &ssearch, &where);
	if (ss == NULL)
		ss = avl_nearest(t, where, AVL_AFTER);

	while (ss != NULL) {
		uint64_t offset = P2ROUNDUP(ss->ss_start, align);

		if (offset + size <= ss->ss_end) {
			*cursor = offset + size;
			return (offset);
		}
		ss = AVL_NEXT(t, ss);
	}

	/*
	 * If we know we've searched the whole map (*cursor == 0), give up.
	 * Otherwise, reset the cursor to the beginning and try again.
	 */
	if (*cursor == 0)
		return (-1ULL);

	*cursor = 0;
	return (metaslab_block_picker(t, cursor, size, align));
}

static void
metaslab_pp_load(space_map_t *sm)
{
	space_seg_t *ss;

	ASSERT(sm->sm_ppd == NULL);
	sm->sm_ppd = kmem_zalloc(64 * sizeof (uint64_t), KM_SLEEP);

	sm->sm_pp_root = kmem_alloc(sizeof (avl_tree_t), KM_SLEEP);
	avl_create(sm->sm_pp_root, metaslab_segsize_compare,
	    sizeof (space_seg_t), offsetof(struct space_seg, ss_pp_node));

	for (ss = avl_first(&sm->sm_root); ss; ss = AVL_NEXT(&sm->sm_root, ss))
		avl_add(sm->sm_pp_root, ss);
}

static void
metaslab_pp_unload(space_map_t *sm)
{
	void *cookie = NULL;

	kmem_free(sm->sm_ppd, 64 * sizeof (uint64_t));
	sm->sm_ppd = NULL;

	while (avl_destroy_nodes(sm->sm_pp_root, &cookie) != NULL) {
		/* tear down the tree */
	}

	avl_destroy(sm->sm_pp_root);
	kmem_free(sm->sm_pp_root, sizeof (avl_tree_t));
	sm->sm_pp_root = NULL;
}

/* ARGSUSED */
static void
metaslab_pp_claim(space_map_t *sm, uint64_t start, uint64_t size)
{
	/* No need to update cursor */
}

/* ARGSUSED */
static void
metaslab_pp_free(space_map_t *sm, uint64_t start, uint64_t size)
{
	/* No need to update cursor */
}

/*
 * Return the maximum contiguous segment within the metaslab.
 */
uint64_t
metaslab_pp_maxsize(space_map_t *sm)
{
	avl_tree_t *t = sm->sm_pp_root;
	space_seg_t *ss;

	if (t == NULL || (ss = avl_last(t)) == NULL)
		return (0ULL);

	return (ss->ss_end - ss->ss_start);
}

/*
 * ==========================================================================
 * The first-fit block allocator
 * ==========================================================================
 */
static uint64_t
metaslab_ff_alloc(space_map_t *sm, uint64_t size, dmu_object_type_t obj_type)
{
	avl_tree_t *t = &sm->sm_root;
	uint64_t align = size & -size;
	uint64_t *cursor = (uint64_t *)sm->sm_ppd + highbit(align) - 1;

	return (metaslab_block_picker(t, cursor, size, align));
}

/* ARGSUSED */
boolean_t
metaslab_ff_fragmented(space_map_t *sm)
{
	return (B_TRUE);
}

static space_map_ops_t metaslab_ff_ops = {
	metaslab_pp_load,
	metaslab_pp_unload,
	metaslab_ff_alloc,
	metaslab_pp_claim,
	metaslab_pp_free,
	metaslab_pp_maxsize,
	metaslab_ff_fragmented
};


static const char *
get_obj_type_name(dmu_object_type_t obj_type)
{
	static const char* type_map[] = {
		"DMU_OT_NONE",
		/* general: */
		"DMU_OT_OBJECT_DIRECTORY",	/* ZAP */
		"DMU_OT_OBJECT_ARRAY",		/* UINT64 */
		"DMU_OT_PACKED_NVLIST",		/* UINT8 (XDR by nvlist_pack/unpack) */
		"DMU_OT_PACKED_NVLIST_SIZE",	/* UINT64 */
		"DMU_OT_BPLIST",			/* UINT64 */
		"DMU_OT_BPLIST_HDR",		/* UINT64 */
		/* spa: */
		"DMU_OT_SPACE_MAP_HEADER",	/* UINT64 */
		"DMU_OT_SPACE_MAP",		/* UINT64 */
		/* zil: */
		"DMU_OT_INTENT_LOG",		/* UINT64 */
		/* dmu: */
		"DMU_OT_DNODE",			/* DNODE */
		"DMU_OT_OBJSET",			/* OBJSET */
		/* dsl: */
		"DMU_OT_DSL_DIR",			/* UINT64 */
		"DMU_OT_DSL_DIR_CHILD_MAP",	/* ZAP */
		"DMU_OT_DSL_DS_SNAP_MAP",		/* ZAP */
		"DMU_OT_DSL_PROPS",		/* ZAP */
		"DMU_OT_DSL_DATASET",		/* UINT64 */
		/* zpl: */
		"DMU_OT_ZNODE",			/* ZNODE */
		"DMU_OT_OLDACL",			/* Old ACL */
		"DMU_OT_PLAIN_FILE_CONTENTS",	/* UINT8 */
		"DMU_OT_DIRECTORY_CONTENTS",	/* ZAP */
		"DMU_OT_MASTER_NODE",		/* ZAP */
		"DMU_OT_UNLINKED_SET",		/* ZAP */
		/* zvol: */
		"DMU_OT_ZVOL",			/* UINT8 */
		"DMU_OT_ZVOL_PROP",		/* ZAP */
		/* other; for testing only! */
		"DMU_OT_PLAIN_OTHER",		/* UINT8 */
		"DMU_OT_UINT64_OTHER",		/* UINT64 */
		"DMU_OT_ZAP_OTHER",		/* ZAP */
		/* new object types: */
		"DMU_OT_ERROR_LOG",		/* ZAP */
		"DMU_OT_SPA_HISTORY",		/* UINT8 */
		"DMU_OT_SPA_HISTORY_OFFSETS",	/* spa_his_phys_t */
		"DMU_OT_POOL_PROPS",		/* ZAP */
		"DMU_OT_DSL_PERMS",		/* ZAP */
		"DMU_OT_ACL",			/* ACL */
		"DMU_OT_SYSACL",			/* SYSACL */
		"DMU_OT_FUID",			/* FUID table (Packed NVLIST UINT8) */
		"DMU_OT_FUID_SIZE",		/* FUID table size UINT64 */
		"DMU_OT_NEXT_CLONES",		/* ZAP */
		"DMU_OT_SCRUB_QUEUE",		/* ZAP */
		"DMU_OT_USERGROUP_USED",		/* ZAP */
		"DMU_OT_USERGROUP_QUOTA",		/* ZAP */
		"DMU_OT_USERREFS",		/* ZAP */
		"DMU_OT_DDT_ZAP",			/* ZAP */
		"DMU_OT_DDT_STATS",		/* ZAP */
		"DMU_OT_NUMTYPES"
	};

	if (obj_type > DMU_OT_NUMTYPES) 
		return "DMU_OT_UNKNOWN";
	
	return type_map[obj_type];
}

/*
 * ==========================================================================
 * Dynamic block allocator -
 * Uses the first fit allocation scheme until space get low and then
 * adjusts to a best fit allocation method. Uses metaslab_df_alloc_threshold
 * and metaslab_df_free_pct to determine when to switch the allocation scheme.
 * ==========================================================================
 */
static uint64_t
metaslab_df_alloc(space_map_t *sm, uint64_t size, dmu_object_type_t obj_type)
{
	avl_tree_t *t = &sm->sm_root;
	uint64_t align = size & -size;
	uint64_t *cursor = (uint64_t *)sm->sm_ppd + highbit(align) - 1;
	uint64_t max_size = metaslab_pp_maxsize(sm);
	int free_pct = sm->sm_space * 100 / sm->sm_size;

	ASSERT(MUTEX_HELD(sm->sm_lock));
	ASSERT3U(avl_numnodes(&sm->sm_root), ==, avl_numnodes(sm->sm_pp_root));

	if (max_size < size)
		return (-1ULL);

	/*
	 * If we're running low on space switch to using the size
	 * sorted AVL tree (best-fit).
	 */
	if (max_size < metaslab_df_alloc_threshold ||
	    free_pct < metaslab_df_free_pct) {
		t = sm->sm_pp_root;
		*cursor = 0;
	}

	uint64_t cur = *cursor;
	uint64_t ret = metaslab_block_picker(t, cursor, size, 1ULL);

#if 0
	static char* hints[] = {"hit ", "near", "far "};
	char *phint;

	static time_t tm_last = 0;
	time_t tm_now;

	time(&tm_now);
	struct tm *tmp = localtime(&tm_now);

	char buf[32] = {0};

	if (difftime(tm_now, tm_last) > 1) {
		strftime(buf, sizeof(buf), "%H:%M:%S", tmp);
	}

	tm_last = tm_now;

	uint64_t d = ret-cur;
	if (d == 0) phint = hints[0];
	else if (abs(d) < 0x10000) phint = hints[1]; // 64K
	else phint = hints[2];

	uint64_t absolute_offset = 0x400000 + ret;
	printf("%8s metaslab_df_alloc(size=0x%04x)=> 0x%016" PRIx64 " -> hint=%s, obj_type=%s\n", buf, (int)size, absolute_offset, phint, get_obj_type_name(obj_type));
#endif

	return ret;
}

static boolean_t
metaslab_df_fragmented(space_map_t *sm)
{
	uint64_t max_size = metaslab_pp_maxsize(sm);
	int free_pct = sm->sm_space * 100 / sm->sm_size;

	if (max_size >= metaslab_df_alloc_threshold &&
	    free_pct >= metaslab_df_free_pct)
		return (B_FALSE);

	return (B_TRUE);
}

static space_map_ops_t metaslab_df_ops = {
	metaslab_pp_load,
	metaslab_pp_unload,
	metaslab_df_alloc,
	metaslab_pp_claim,
	metaslab_pp_free,
	metaslab_pp_maxsize,
	metaslab_df_fragmented
};

/*
 * ==========================================================================
 * Other experimental allocators
 * ==========================================================================
 */
static uint64_t
metaslab_cdf_alloc(space_map_t *sm, uint64_t size, dmu_object_type_t obj_type)
{
	avl_tree_t *t = &sm->sm_root;
	uint64_t *cursor = (uint64_t *)sm->sm_ppd;
	uint64_t *extent_end = (uint64_t *)sm->sm_ppd + 1;
	uint64_t max_size = metaslab_pp_maxsize(sm);
	uint64_t rsize = size;
	uint64_t offset = 0;

	ASSERT(MUTEX_HELD(sm->sm_lock));
	ASSERT3U(avl_numnodes(&sm->sm_root), ==, avl_numnodes(sm->sm_pp_root));

	if (max_size < size)
		return (-1ULL);

	ASSERT3U(*extent_end, >=, *cursor);

	/*
	 * If we're running low on space switch to using the size
	 * sorted AVL tree (best-fit).
	 */
	if ((*cursor + size) > *extent_end) {

		t = sm->sm_pp_root;
		*cursor = *extent_end = 0;

		if (max_size > 2 * SPA_MAXBLOCKSIZE)
			rsize = MIN(metaslab_min_alloc_size, max_size);
		offset = metaslab_block_picker(t, extent_end, rsize, 1ULL);
		if (offset != -1)
			*cursor = offset + size;
	} else {
		offset = metaslab_block_picker(t, cursor, rsize, 1ULL);
	}
	ASSERT3U(*cursor, <=, *extent_end);
	return (offset);
}

static boolean_t
metaslab_cdf_fragmented(space_map_t *sm)
{
	uint64_t max_size = metaslab_pp_maxsize(sm);

	if (max_size > (metaslab_min_alloc_size * 10))
		return (B_FALSE);
	return (B_TRUE);
}

static space_map_ops_t metaslab_cdf_ops = {
	metaslab_pp_load,
	metaslab_pp_unload,
	metaslab_cdf_alloc,
	metaslab_pp_claim,
	metaslab_pp_free,
	metaslab_pp_maxsize,
	metaslab_cdf_fragmented
};

static uint64_t
metaslab_ndf_alloc(space_map_t *sm, uint64_t size, dmu_object_type_t obj_type)
{
	avl_tree_t *t = &sm->sm_root;
	avl_index_t where;
	space_seg_t *ss, ssearch;
	uint64_t *cursor = (uint64_t *)sm->sm_ppd;
	uint64_t max_size = metaslab_pp_maxsize(sm);

	ASSERT(MUTEX_HELD(sm->sm_lock));
	ASSERT3U(avl_numnodes(&sm->sm_root), ==, avl_numnodes(sm->sm_pp_root));

	if (max_size < size)
		return (-1ULL);

	ssearch.ss_start = *cursor;
	ssearch.ss_end = *cursor + size;

	ss = avl_find(t, &ssearch, &where);
	if (ss == NULL || (ss->ss_start + size > ss->ss_end)) {
		t = sm->sm_pp_root;

		if (max_size > 2 * SPA_MAXBLOCKSIZE)
			size = MIN(metaslab_min_alloc_size, max_size);

		ssearch.ss_start = 0;
		ssearch.ss_end = size;
		ss = avl_find(t, &ssearch, &where);
		if (ss == NULL)
			ss = avl_nearest(t, where, AVL_AFTER);
		ASSERT(ss != NULL);
	}

	if (ss != NULL) {
		if (ss->ss_start + size <= ss->ss_end) {
			*cursor = ss->ss_start + size;
			return (ss->ss_start);
		}
	}
	return (-1ULL);
}

static boolean_t
metaslab_ndf_fragmented(space_map_t *sm)
{
	uint64_t max_size = metaslab_pp_maxsize(sm);

	if (max_size > (metaslab_min_alloc_size * 10))
		return (B_FALSE);
	return (B_TRUE);
}


static space_map_ops_t metaslab_ndf_ops = {
	metaslab_pp_load,
	metaslab_pp_unload,
	metaslab_ndf_alloc,
	metaslab_pp_claim,
	metaslab_pp_free,
	metaslab_pp_maxsize,
	metaslab_ndf_fragmented
};

space_map_ops_t *zfs_metaslab_ops = &metaslab_df_ops;

/*
 * ==========================================================================
 * Metaslabs
 * ==========================================================================
 */
metaslab_t *
metaslab_init(metaslab_group_t *mg, space_map_obj_t *smo,
	uint64_t start, uint64_t size, uint64_t txg)
{
	vdev_t *vd = mg->mg_vd;
	metaslab_t *msp;

	msp = kmem_zalloc(sizeof (metaslab_t), KM_SLEEP);
	mutex_init(&msp->ms_lock, NULL, MUTEX_DEFAULT, NULL);

	msp->ms_smo_syncing = *smo;

	/*
	 * We create the main space map here, but we don't create the
	 * allocmaps and freemaps until metaslab_sync_done().  This serves
	 * two purposes: it allows metaslab_sync_done() to detect the
	 * addition of new space; and for debugging, it ensures that we'd
	 * data fault on any attempt to use this metaslab before it's ready.
	 */
	space_map_create(&msp->ms_map, start, size,
	    vd->vdev_ashift, &msp->ms_lock);

	metaslab_group_add(mg, msp);

	if (metaslab_debug && smo->smo_object != 0) {
		mutex_enter(&msp->ms_lock);
		VERIFY(space_map_load(&msp->ms_map, mg->mg_class->mc_ops,
		    SM_FREE, smo, spa_meta_objset(vd->vdev_spa)) == 0);
		mutex_exit(&msp->ms_lock);
	}

	/*
	 * If we're opening an existing pool (txg == 0) or creating
	 * a new one (txg == TXG_INITIAL), all space is available now.
	 * If we're adding space to an existing pool, the new space
	 * does not become available until after this txg has synced.
	 */
	if (txg <= TXG_INITIAL)
		metaslab_sync_done(msp, 0);

	if (txg != 0) {
		vdev_dirty(vd, 0, NULL, txg);
		vdev_dirty(vd, VDD_METASLAB, msp, txg);
	}

	return (msp);
}

void
metaslab_fini(metaslab_t *msp)
{
	metaslab_group_t *mg = msp->ms_group;

	vdev_space_update(mg->mg_vd,
	    -msp->ms_smo.smo_alloc, 0, -msp->ms_map.sm_size);

	metaslab_group_remove(mg, msp);

	mutex_enter(&msp->ms_lock);

	space_map_unload(&msp->ms_map);
	space_map_destroy(&msp->ms_map);

	for (int t = 0; t < TXG_SIZE; t++) {
		space_map_destroy(&msp->ms_allocmap[t]);
		space_map_destroy(&msp->ms_freemap[t]);
	}

	for (int t = 0; t < TXG_DEFER_SIZE; t++)
		space_map_destroy(&msp->ms_defermap[t]);

	ASSERT3S(msp->ms_deferspace, ==, 0);

	mutex_exit(&msp->ms_lock);
	mutex_destroy(&msp->ms_lock);

	kmem_free(msp, sizeof (metaslab_t));
}

#define	METASLAB_WEIGHT_PRIMARY		(1ULL << 63)
#define	METASLAB_WEIGHT_SECONDARY	(1ULL << 62)
#define	METASLAB_ACTIVE_MASK		\
	(METASLAB_WEIGHT_PRIMARY | METASLAB_WEIGHT_SECONDARY)

static uint64_t
metaslab_weight(metaslab_t *msp)
{
	metaslab_group_t *mg = msp->ms_group;
	space_map_t *sm = &msp->ms_map;
	space_map_obj_t *smo = &msp->ms_smo;
	vdev_t *vd = mg->mg_vd;
	uint64_t weight, space;

	ASSERT(MUTEX_HELD(&msp->ms_lock));

	/*
	 * The baseline weight is the metaslab's free space.
	 */
	space = sm->sm_size - smo->smo_alloc;
	weight = space;

	/*
	 * Modern disks have uniform bit density and constant angular velocity.
	 * Therefore, the outer recording zones are faster (higher bandwidth)
	 * than the inner zones by the ratio of outer to inner track diameter,
	 * which is typically around 2:1.  We account for this by assigning
	 * higher weight to lower metaslabs (multiplier ranging from 2x to 1x).
	 * In effect, this means that we'll select the metaslab with the most
	 * free bandwidth rather than simply the one with the most free space.
	 */
	weight = 2 * weight -
	    ((sm->sm_start >> vd->vdev_ms_shift) * weight) / vd->vdev_ms_count;
	ASSERT(weight >= space && weight <= 2 * space);

	/*
	 * For locality, assign higher weight to metaslabs which have
	 * a lower offset than what we've already activated.
	 */
	if (sm->sm_start <= mg->mg_bonus_area)
		weight *= (metaslab_smo_bonus_pct / 100);
	ASSERT(weight >= space &&
	    weight <= 2 * (metaslab_smo_bonus_pct / 100) * space);

	if (sm->sm_loaded && !sm->sm_ops->smop_fragmented(sm)) {
		/*
		 * If this metaslab is one we're actively using, adjust its
		 * weight to make it preferable to any inactive metaslab so
		 * we'll polish it off.
		 */
		weight |= (msp->ms_weight & METASLAB_ACTIVE_MASK);
	}
	return (weight);
}

static void
metaslab_prefetch(metaslab_group_t *mg)
{
	spa_t *spa = mg->mg_vd->vdev_spa;
	metaslab_t *msp;
	avl_tree_t *t = &mg->mg_metaslab_tree;
	int m;

	mutex_enter(&mg->mg_lock);

	/*
	 * Prefetch the next potential metaslabs
	 */
	for (msp = avl_first(t), m = 0; msp; msp = AVL_NEXT(t, msp), m++) {
		space_map_t *sm = &msp->ms_map;
		space_map_obj_t *smo = &msp->ms_smo;

		/* If we have reached our prefetch limit then we're done */
		if (m >= metaslab_prefetch_limit)
			break;

		if (!sm->sm_loaded && smo->smo_object != 0) {
			mutex_exit(&mg->mg_lock);
			dmu_prefetch(spa_meta_objset(spa), smo->smo_object,
			    0ULL, smo->smo_objsize);
			mutex_enter(&mg->mg_lock);
		}
	}
	mutex_exit(&mg->mg_lock);
}

static int
metaslab_activate(metaslab_t *msp, uint64_t activation_weight, uint64_t size)
{
	metaslab_group_t *mg = msp->ms_group;
	space_map_t *sm = &msp->ms_map;
	space_map_ops_t *sm_ops = msp->ms_group->mg_class->mc_ops;

	ASSERT(MUTEX_HELD(&msp->ms_lock));

	if ((msp->ms_weight & METASLAB_ACTIVE_MASK) == 0) {
		space_map_load_wait(sm);
		if (!sm->sm_loaded) {
			int error = space_map_load(sm, sm_ops, SM_FREE,
			    &msp->ms_smo,
			    spa_meta_objset(msp->ms_group->mg_vd->vdev_spa));
			if (error)  {
				metaslab_group_sort(msp->ms_group, msp, 0);
				return (error);
			}
			for (int t = 0; t < TXG_DEFER_SIZE; t++)
				space_map_walk(&msp->ms_defermap[t],
				    space_map_claim, sm);

		}

		/*
		 * Track the bonus area as we activate new metaslabs.
		 */
		if (sm->sm_start > mg->mg_bonus_area) {
			mutex_enter(&mg->mg_lock);
			mg->mg_bonus_area = sm->sm_start;
			mutex_exit(&mg->mg_lock);
		}

		/*
		 * If we were able to load the map then make sure
		 * that this map is still able to satisfy our request.
		 */
		if (msp->ms_weight < size)
			return (ENOSPC);

		metaslab_group_sort(msp->ms_group, msp,
		    msp->ms_weight | activation_weight);
	}
	ASSERT(sm->sm_loaded);
	ASSERT(msp->ms_weight & METASLAB_ACTIVE_MASK);

	return (0);
}

static void
metaslab_passivate(metaslab_t *msp, uint64_t size)
{
	/*
	 * If size < SPA_MINBLOCKSIZE, then we will not allocate from
	 * this metaslab again.  In that case, it had better be empty,
	 * or we would be leaving space on the table.
	 */
	ASSERT(size >= SPA_MINBLOCKSIZE || msp->ms_map.sm_space == 0);
	metaslab_group_sort(msp->ms_group, msp, MIN(msp->ms_weight, size));
	ASSERT((msp->ms_weight & METASLAB_ACTIVE_MASK) == 0);
}

/*
 * Write a metaslab to disk in the context of the specified transaction group.
 */
void
metaslab_sync(metaslab_t *msp, uint64_t txg)
{
	vdev_t *vd = msp->ms_group->mg_vd;
	spa_t *spa = vd->vdev_spa;
	objset_t *mos = spa_meta_objset(spa);
	space_map_t *allocmap = &msp->ms_allocmap[txg & TXG_MASK];
	space_map_t *freemap = &msp->ms_freemap[txg & TXG_MASK];
	space_map_t *freed_map = &msp->ms_freemap[TXG_CLEAN(txg) & TXG_MASK];
	space_map_t *sm = &msp->ms_map;
	space_map_obj_t *smo = &msp->ms_smo_syncing;
	dmu_buf_t *db;
	dmu_tx_t *tx;

	ASSERT(!vd->vdev_ishole);

	if (allocmap->sm_space == 0 && freemap->sm_space == 0)
		return;

	/*
	 * The only state that can actually be changing concurrently with
	 * metaslab_sync() is the metaslab's ms_map.  No other thread can
	 * be modifying this txg's allocmap, freemap, freed_map, or smo.
	 * Therefore, we only hold ms_lock to satify space_map ASSERTs.
	 * We drop it whenever we call into the DMU, because the DMU
	 * can call down to us (e.g. via zio_free()) at any time.
	 */

	tx = dmu_tx_create_assigned(spa_get_dsl(spa), txg);

	if (smo->smo_object == 0) {
		ASSERT(smo->smo_objsize == 0);
		ASSERT(smo->smo_alloc == 0);
		smo->smo_object = dmu_object_alloc(mos,
		    DMU_OT_SPACE_MAP, 1 << SPACE_MAP_BLOCKSHIFT,
		    DMU_OT_SPACE_MAP_HEADER, sizeof (*smo), tx);
		ASSERT(smo->smo_object != 0);
		dmu_write(mos, vd->vdev_ms_array, sizeof (uint64_t) *
		    (sm->sm_start >> vd->vdev_ms_shift),
		    sizeof (uint64_t), &smo->smo_object, tx);
	}

	mutex_enter(&msp->ms_lock);

	space_map_walk(freemap, space_map_add, freed_map);

	if (sm->sm_loaded && spa_sync_pass(spa) == 1 && smo->smo_objsize >=
	    2 * sizeof (uint64_t) * avl_numnodes(&sm->sm_root)) {
		/*
		 * The in-core space map representation is twice as compact
		 * as the on-disk one, so it's time to condense the latter
		 * by generating a pure allocmap from first principles.
		 *
		 * This metaslab is 100% allocated,
		 * minus the content of the in-core map (sm),
		 * minus what's been freed this txg (freed_map),
		 * minus deferred frees (ms_defermap[]),
		 * minus allocations from txgs in the future
		 * (because they haven't been committed yet).
		 */
		space_map_vacate(allocmap, NULL, NULL);
		space_map_vacate(freemap, NULL, NULL);

		space_map_add(allocmap, allocmap->sm_start, allocmap->sm_size, METASLAB_ALLOC_UNKNOWN);

		space_map_walk(sm, space_map_remove, allocmap);
		space_map_walk(freed_map, space_map_remove, allocmap);

		for (int t = 0; t < TXG_DEFER_SIZE; t++)
			space_map_walk(&msp->ms_defermap[t],
			    space_map_remove, allocmap);

		for (int t = 1; t < TXG_CONCURRENT_STATES; t++)
			space_map_walk(&msp->ms_allocmap[(txg + t) & TXG_MASK],
			    space_map_remove, allocmap);

		mutex_exit(&msp->ms_lock);
		space_map_truncate(smo, mos, tx);
		mutex_enter(&msp->ms_lock);
	}

	space_map_sync(allocmap, SM_ALLOC, smo, mos, tx);
	space_map_sync(freemap, SM_FREE, smo, mos, tx);

	mutex_exit(&msp->ms_lock);

	VERIFY(0 == dmu_bonus_hold(mos, smo->smo_object, FTAG, &db));
	dmu_buf_will_dirty(db, tx);
	ASSERT3U(db->db_size, >=, sizeof (*smo));
	bcopy(smo, db->db_data, sizeof (*smo));
	dmu_buf_rele(db, FTAG);

	dmu_tx_commit(tx);
}

/*
 * Called after a transaction group has completely synced to mark
 * all of the metaslab's free space as usable.
 */
void
metaslab_sync_done(metaslab_t *msp, uint64_t txg)
{
	space_map_obj_t *smo = &msp->ms_smo;
	space_map_obj_t *smosync = &msp->ms_smo_syncing;
	space_map_t *sm = &msp->ms_map;
	space_map_t *freed_map = &msp->ms_freemap[TXG_CLEAN(txg) & TXG_MASK];
	space_map_t *defer_map = &msp->ms_defermap[txg % TXG_DEFER_SIZE];
	metaslab_group_t *mg = msp->ms_group;
	vdev_t *vd = mg->mg_vd;
	int64_t alloc_delta, defer_delta;

	ASSERT(!vd->vdev_ishole);

	mutex_enter(&msp->ms_lock);

	/*
	 * If this metaslab is just becoming available, initialize its
	 * allocmaps and freemaps and add its capacity to the vdev.
	 */
	if (freed_map->sm_size == 0) {
		for (int t = 0; t < TXG_SIZE; t++) {
			space_map_create(&msp->ms_allocmap[t], sm->sm_start,
			    sm->sm_size, sm->sm_shift, sm->sm_lock);
			space_map_create(&msp->ms_freemap[t], sm->sm_start,
			    sm->sm_size, sm->sm_shift, sm->sm_lock);
		}

		for (int t = 0; t < TXG_DEFER_SIZE; t++)
			space_map_create(&msp->ms_defermap[t], sm->sm_start,
			    sm->sm_size, sm->sm_shift, sm->sm_lock);

		vdev_space_update(vd, 0, 0, sm->sm_size);
	}

	alloc_delta = smosync->smo_alloc - smo->smo_alloc;
	defer_delta = freed_map->sm_space - defer_map->sm_space;

	vdev_space_update(vd, alloc_delta + defer_delta, defer_delta, 0);

	ASSERT(msp->ms_allocmap[txg & TXG_MASK].sm_space == 0);
	ASSERT(msp->ms_freemap[txg & TXG_MASK].sm_space == 0);

	/*
	 * If there's a space_map_load() in progress, wait for it to complete
	 * so that we have a consistent view of the in-core space map.
	 * Then, add defer_map (oldest deferred frees) to this map and
	 * transfer freed_map (this txg's frees) to defer_map.
	 */
	space_map_load_wait(sm);
	space_map_vacate(defer_map, sm->sm_loaded ? space_map_free : NULL, sm);
	space_map_vacate(freed_map, space_map_add, defer_map);

	*smo = *smosync;

	msp->ms_deferspace += defer_delta;
	ASSERT3S(msp->ms_deferspace, >=, 0);
	ASSERT3S(msp->ms_deferspace, <=, sm->sm_size);
	if (msp->ms_deferspace != 0) {
		/*
		 * Keep syncing this metaslab until all deferred frees
		 * are back in circulation.
		 */
		vdev_dirty(vd, VDD_METASLAB, msp, txg + 1);
	}

	/*
	 * If the map is loaded but no longer active, evict it as soon as all
	 * future allocations have synced.  (If we unloaded it now and then
	 * loaded a moment later, the map wouldn't reflect those allocations.)
	 */
	if (sm->sm_loaded && (msp->ms_weight & METASLAB_ACTIVE_MASK) == 0) {
		int evictable = 1;

		for (int t = 1; t < TXG_CONCURRENT_STATES; t++)
			if (msp->ms_allocmap[(txg + t) & TXG_MASK].sm_space)
				evictable = 0;

		if (evictable && !metaslab_debug)
			space_map_unload(sm);
	}

	metaslab_group_sort(mg, msp, metaslab_weight(msp));

	mutex_exit(&msp->ms_lock);
}

void
metaslab_sync_reassess(metaslab_group_t *mg)
{
	vdev_t *vd = mg->mg_vd;

	/*
	 * Re-evaluate all metaslabs which have lower offsets than the
	 * bonus area.
	 */
	for (int m = 0; m < vd->vdev_ms_count; m++) {
		metaslab_t *msp = vd->vdev_ms[m];

		if (msp->ms_map.sm_start > mg->mg_bonus_area)
			break;

		mutex_enter(&msp->ms_lock);
		metaslab_group_sort(mg, msp, metaslab_weight(msp));
		mutex_exit(&msp->ms_lock);
	}

	/*
	 * Prefetch the next potential metaslabs
	 */
	metaslab_prefetch(mg);
}

static uint64_t
metaslab_distance(metaslab_t *msp, dva_t *dva)
{
	uint64_t ms_shift = msp->ms_group->mg_vd->vdev_ms_shift;
	uint64_t offset = DVA_GET_OFFSET(dva) >> ms_shift;
	uint64_t start = msp->ms_map.sm_start >> ms_shift;

	if (msp->ms_group->mg_vd->vdev_id != DVA_GET_VDEV(dva))
		return (1ULL << 63);

	if (offset < start)
		return ((start - offset) << ms_shift);
	if (offset > start)
		return ((offset - start) << ms_shift);
	return (0);
}

struct vab_iter {
	metaslab_t	*vi_msp;
	size_t		vi_count;
	size_t		vi_index;
	vab_free_segment_t vi_segments[];
};

static int
metaslab_load_space_map_locked(metaslab_t *msp)
{
	space_map_t *sm = &msp->ms_map;
	metaslab_group_t *mg = msp->ms_group;
	space_map_ops_t *ops = mg->mg_class->mc_ops;
	spa_t *spa = mg->mg_vd->vdev_spa;

	ASSERT(MUTEX_HELD(&msp->ms_lock));

	space_map_load_wait(sm);
	if (sm->sm_loaded)
		return (0);

	int error = space_map_load(sm, ops, SM_FREE, &msp->ms_smo,
	    spa_meta_objset(spa));
	if (error != 0)
		return (error);

	for (int t = 0; t < TXG_DEFER_SIZE; t++)
		space_map_walk(&msp->ms_defermap[t], space_map_claim, sm);

	return (0);
}

static uint64_t
metaslab_alloc_from_hint(metaslab_t *msp, uint64_t offset, uint64_t size,
    uint64_t txg, dmu_object_type_t obj_type)
{
	uint64_t result = -1ULL;
	vdev_t *vd = msp->ms_group->mg_vd;

	mutex_enter(&msp->ms_lock);

	if (metaslab_activate(msp, METASLAB_WEIGHT_PRIMARY, size) != 0) {
		mutex_exit(&msp->ms_lock);
		return (result);
	}

	if (!space_map_contains(&msp->ms_map, offset, size)) {
		mutex_exit(&msp->ms_lock);
		return (result);
	}

	space_map_remove(&msp->ms_map, offset, size, obj_type);

	if (msp->ms_allocmap[txg & TXG_MASK].sm_space == 0)
		vdev_dirty(vd, VDD_METASLAB, msp, txg);

	space_map_add(&msp->ms_allocmap[txg & TXG_MASK], offset, size, obj_type);

	mutex_exit(&msp->ms_lock);

	return (offset);
}

static void
metaslab_sort_segments_by_size(vab_free_segment_t *segments, size_t count)
{
	for (size_t i = 1; i < count; i++) {
		vab_free_segment_t tmp = segments[i];
		size_t j = i;
		while (j > 0 && segments[j - 1].vfs_size < tmp.vfs_size) {
			segments[j] = segments[j - 1];
			j--;
		}
		segments[j] = tmp;
	}
}

static void *
metaslab_backend_first_region(vdev_t *vd)
{
	for (uint64_t i = 0; i < vd->vdev_ms_count; i++) {
		if (vd->vdev_ms[i] != NULL)
			return (vd->vdev_ms[i]);
	}
	return (NULL);
}

static void *
metaslab_backend_next_region(void *region_handle)
{
	metaslab_t *msp = region_handle;
	vdev_t *vd = msp->ms_group->mg_vd;
	uint64_t shift = vd->vdev_ms_shift;
	uint64_t start = msp->ms_map.sm_start;
	uint64_t idx = start >> shift;

	for (uint64_t i = idx + 1; i < vd->vdev_ms_count; i++) {
		if (vd->vdev_ms[i] != NULL)
			return (vd->vdev_ms[i]);
	}

	return (NULL);
}

static int
metaslab_backend_get_region_stats(void *region_handle,
    vab_region_stats_t *stats_out)
{
	metaslab_t *msp = region_handle;

	if (msp == NULL || stats_out == NULL)
		return (EINVAL);

	bzero(stats_out, sizeof (*stats_out));

	mutex_enter(&msp->ms_lock);
	stats_out->vrs_start = msp->ms_map.sm_start;
	stats_out->vrs_size = msp->ms_map.sm_size;
	stats_out->vrs_allocated = msp->ms_smo.smo_alloc;
	stats_out->vrs_loaded = msp->ms_map.sm_loaded;
	stats_out->vrs_active =
	    (msp->ms_weight & METASLAB_ACTIVE_MASK) != 0;

	if (msp->ms_map.sm_loaded) {
		stats_out->vrs_free_space = msp->ms_map.sm_space;
		stats_out->vrs_max_segment =
		    space_map_maxsize(&msp->ms_map);
	} else {
		uint64_t free_estimate = stats_out->vrs_size -
		    stats_out->vrs_allocated;
		stats_out->vrs_free_space = free_estimate;
		stats_out->vrs_max_segment = free_estimate;
	}
	mutex_exit(&msp->ms_lock);

	return (0);
}

static int
metaslab_backend_alloc(void *region_handle, uint64_t offset, uint64_t size,
    dmu_object_type_t obj_type, uint64_t txg)
{
	if (region_handle == NULL || size == 0)
		return (EINVAL);

	uint64_t result = metaslab_alloc_from_hint(region_handle, offset,
	    size, txg, obj_type);

	return (result == -1ULL ? ENOSPC : 0);
}

static boolean_t
metaslab_backend_is_free(void *region_handle, uint64_t offset, uint64_t size)
{
	metaslab_t *msp = region_handle;
	boolean_t result = B_FALSE;

	if (msp == NULL || size == 0)
		return (B_FALSE);

	mutex_enter(&msp->ms_lock);
	if (metaslab_load_space_map_locked(msp) == 0)
		result = space_map_contains(&msp->ms_map, offset, size);
	mutex_exit(&msp->ms_lock);

	return (result);
}

static int
metaslab_backend_iter_create(void *region_handle, vab_iter_order_t order,
    uint64_t min_size, vab_iter_t **iter_out)
{
	metaslab_t *msp = region_handle;
	vab_iter_t *iter;
	size_t count = 0;
	space_seg_t *ss;

	if (msp == NULL || iter_out == NULL)
		return (EINVAL);

	mutex_enter(&msp->ms_lock);
	int error = metaslab_load_space_map_locked(msp);
	if (error != 0) {
		mutex_exit(&msp->ms_lock);
		return (error);
	}

	for (ss = avl_first(&msp->ms_map.sm_root);
	    ss != NULL; ss = AVL_NEXT(&msp->ms_map.sm_root, ss)) {
		uint64_t seg_size = ss->ss_end - ss->ss_start;
		if (seg_size >= min_size)
			count++;
	}

	size_t alloc_size = sizeof (vab_iter_t) +
	    (count * sizeof (vab_free_segment_t));
	iter = kmem_zalloc(alloc_size, KM_SLEEP);
	iter->vi_msp = msp;
	iter->vi_count = count;
	iter->vi_index = 0;

	if (count != 0) {
		size_t idx = 0;
		for (ss = avl_first(&msp->ms_map.sm_root);
		    ss != NULL; ss = AVL_NEXT(&msp->ms_map.sm_root, ss)) {
			uint64_t seg_size = ss->ss_end - ss->ss_start;
			if (seg_size < min_size)
				continue;
			iter->vi_segments[idx].vfs_offset = ss->ss_start;
			iter->vi_segments[idx].vfs_size = seg_size;
			idx++;
		}

		if (order == VAB_ITER_ORDER_SIZE)
			metaslab_sort_segments_by_size(iter->vi_segments, iter->vi_count);
	}

	mutex_exit(&msp->ms_lock);

	*iter_out = iter;
	return (0);
}

static bool
metaslab_backend_iter_next(vab_iter_t *iter, vab_free_segment_t *segment_out)
{
	if (iter == NULL || segment_out == NULL)
		return (false);

	if (iter->vi_index >= iter->vi_count)
		return (false);

	*segment_out = iter->vi_segments[iter->vi_index++];
	return (true);
}

static void
metaslab_backend_iter_destroy(vab_iter_t *iter)
{
	if (iter == NULL)
		return;

	size_t alloc_size = sizeof (vab_iter_t) +
	    (iter->vi_count * sizeof (vab_free_segment_t));
	kmem_free(iter, alloc_size);
}

static const vdev_alloc_backend_ops_t metaslab_alloc_backend_ops_impl = {
	.vab_first_region = metaslab_backend_first_region,
	.vab_next_region = metaslab_backend_next_region,
	.vab_get_region_stats = metaslab_backend_get_region_stats,
	.vab_alloc = metaslab_backend_alloc,
	.vab_is_free = metaslab_backend_is_free,
	.vab_iter_create = metaslab_backend_iter_create,
	.vab_iter_next = metaslab_backend_iter_next,
	.vab_iter_destroy = metaslab_backend_iter_destroy,
};

const vdev_alloc_backend_ops_t *
metaslab_get_alloc_backend_ops(void)
{
	return (&metaslab_alloc_backend_ops_impl);
}

static uint64_t
metaslab_group_alloc(metaslab_group_t *mg, uint64_t size, uint64_t txg,
    uint64_t min_distance, dva_t *dva, int d, dmu_object_type_t obj_type,
    metaslab_t **selected_msp)
{
	metaslab_t *msp = NULL;
	uint64_t offset = -1ULL;
	avl_tree_t *t = &mg->mg_metaslab_tree;
	uint64_t activation_weight;
	uint64_t target_distance;
	int i;

	activation_weight = METASLAB_WEIGHT_PRIMARY;
	for (i = 0; i < d; i++) {
		if (DVA_GET_VDEV(&dva[i]) == mg->mg_vd->vdev_id) {
			activation_weight = METASLAB_WEIGHT_SECONDARY;
			break;
		}
	}

	for (;;) {
		boolean_t was_active;

		mutex_enter(&mg->mg_lock);
		for (msp = avl_first(t); msp; msp = AVL_NEXT(t, msp)) {
			if (msp->ms_weight < size) {
				mutex_exit(&mg->mg_lock);
				return (-1ULL);
			}

			was_active = msp->ms_weight & METASLAB_ACTIVE_MASK;
			if (activation_weight == METASLAB_WEIGHT_PRIMARY)
				break;

			target_distance = min_distance +
			    (msp->ms_smo.smo_alloc ? 0 : min_distance >> 1);

			for (i = 0; i < d; i++)
				if (metaslab_distance(msp, &dva[i]) <
				    target_distance)
					break;
			if (i == d)
				break;
		}
		mutex_exit(&mg->mg_lock);
		if (msp == NULL)
			return (-1ULL);

		mutex_enter(&msp->ms_lock);

		/*
		 * Ensure that the metaslab we have selected is still
		 * capable of handling our request. It's possible that
		 * another thread may have changed the weight while we
		 * were blocked on the metaslab lock.
		 */
		if (msp->ms_weight < size || (was_active &&
		    !(msp->ms_weight & METASLAB_ACTIVE_MASK) &&
		    activation_weight == METASLAB_WEIGHT_PRIMARY)) {
			mutex_exit(&msp->ms_lock);
			continue;
		}

		if ((msp->ms_weight & METASLAB_WEIGHT_SECONDARY) &&
		    activation_weight == METASLAB_WEIGHT_PRIMARY) {
			metaslab_passivate(msp,
			    msp->ms_weight & ~METASLAB_ACTIVE_MASK);
			mutex_exit(&msp->ms_lock);
			continue;
		}

		if (metaslab_activate(msp, activation_weight, size) != 0) {
			mutex_exit(&msp->ms_lock);
			continue;
		}

		if ((offset = space_map_alloc(&msp->ms_map, size, obj_type)) != -1ULL)
			break;

		metaslab_passivate(msp, space_map_maxsize(&msp->ms_map));

		mutex_exit(&msp->ms_lock);
	}

	if (msp->ms_allocmap[txg & TXG_MASK].sm_space == 0)
		vdev_dirty(mg->mg_vd, VDD_METASLAB, msp, txg);

	space_map_add(&msp->ms_allocmap[txg & TXG_MASK], offset, size, obj_type);

	mutex_exit(&msp->ms_lock);

	if (selected_msp != NULL)
		*selected_msp = msp;

	return (offset);
}

/*
 * Allocate a block for the specified i/o.
 */
static int
metaslab_alloc_dva(spa_t *spa, metaslab_class_t *mc, uint64_t psize,
    dva_t *dva, int d, dva_t *hintdva, uint64_t txg, int flags,
    dmu_object_type_t obj_type, zio_t *zio)
{
	metaslab_group_t *mg, *rotor;
	vdev_t *vd;
	int dshift = 3;
	int all_zero;
	int zio_lock = B_FALSE;
	boolean_t allocatable;
	uint64_t offset = -1ULL;
	uint64_t asize;
	uint64_t distance;
	alloc_bias_ops_t *bias_ops = (zio != NULL) ? zio->io_alloc_bias_ops : NULL;
	uint64_t bias_key = (zio != NULL) ? zio->io_alloc_bias_key : 0;

	ASSERT(!DVA_IS_VALID(&dva[d]));

	/*
	 * For testing, make some blocks above a certain size be gang blocks.
	 */
	if (psize >= metaslab_gang_bang && (lbolt & 3) == 0)
		return (ENOSPC);

	/*
	 * Start at the rotor and loop through all mgs until we find something.
	 * Note that there's no locking on mc_rotor or mc_aliquot because
	 * nothing actually breaks if we miss a few updates -- we just won't
	 * allocate quite as evenly.  It all balances out over time.
	 *
	 * If we are doing ditto or log blocks, try to spread them across
	 * consecutive vdevs.  If we're forced to reuse a vdev before we've
	 * allocated all of our ditto blocks, then try and spread them out on
	 * that vdev as much as possible.  If it turns out to not be possible,
	 * gradually lower our standards until anything becomes acceptable.
	 * Also, allocating on consecutive vdevs (as opposed to random vdevs)
	 * gives us hope of containing our fault domains to something we're
	 * able to reason about.  Otherwise, any two top-level vdev failures
	 * will guarantee the loss of data.  With consecutive allocation,
	 * only two adjacent top-level vdev failures will result in data loss.
	 *
	 * If we are doing gang blocks (hintdva is non-NULL), try to keep
	 * ourselves on the same vdev as our gang block header.  That
	 * way, we can hope for locality in vdev_cache, plus it makes our
	 * fault domains something tractable.
	 */
	if (hintdva) {
		vd = vdev_lookup_top(spa, DVA_GET_VDEV(&hintdva[d]));

		/*
		 * It's possible the vdev we're using as the hint no
		 * longer exists (i.e. removed). Consult the rotor when
		 * all else fails.
		 */
		if (vd != NULL) {
			mg = vd->vdev_mg;

			if (flags & METASLAB_HINTBP_AVOID &&
			    mg->mg_next != NULL)
				mg = mg->mg_next;
		} else {
			mg = mc->mc_rotor;
		}
	} else if (d != 0) {
		vd = vdev_lookup_top(spa, DVA_GET_VDEV(&dva[d - 1]));
		mg = vd->vdev_mg->mg_next;
	} else {
		mg = mc->mc_rotor;
	}

	/*
	 * If the hint put us into the wrong metaslab class, or into a
	 * metaslab group that has been passivated, just follow the rotor.
	 */
	if (mg->mg_class != mc || mg->mg_activation_count <= 0)
		mg = mc->mc_rotor;

	rotor = mg;
top:
	all_zero = B_TRUE;
	do {
		ASSERT(mg->mg_activation_count == 1);

		vd = mg->mg_vd;

		/*
		 * Don't allocate from faulted devices.
		 */
		if (zio_lock) {
			spa_config_enter(spa, SCL_ZIO, FTAG, RW_READER);
			allocatable = vdev_allocatable(vd);
			spa_config_exit(spa, SCL_ZIO, FTAG);
		} else {
			allocatable = vdev_allocatable(vd);
		}
		if (!allocatable)
			goto next;

		/*
		 * Avoid writing single-copy data to a failing vdev
		 */
		if ((vd->vdev_stat.vs_write_errors > 0 ||
		    vd->vdev_state < VDEV_STATE_HEALTHY) &&
		    d == 0 && dshift == 3) {
			all_zero = B_FALSE;
			goto next;
		}

		ASSERT(mg->mg_class == mc);

		distance = vd->vdev_asize >> dshift;
		if (distance <= (1ULL << vd->vdev_ms_shift))
			distance = 0;
		else
			all_zero = B_FALSE;

		asize = vdev_psize_to_asize(vd, psize);
		ASSERT(P2PHASE(asize, 1ULL << vd->vdev_ashift) == 0);

		boolean_t create_ctx = B_FALSE;
		alloc_bias_context_t *bias_ctx = NULL;
		boolean_t bias_hint_valid = B_FALSE;
		alloc_bias_hint_t bias_hint;
		uint64_t bias_stream_id = 0;

		if (bias_ops != NULL && bias_key != 0) {
			alloc_bias_req_t req = { 0 };
			boolean_t eligible = B_TRUE;

			req.abr_io_req = zio;
			req.abr_size = asize;
			req.abr_vdev = vd;
			req.abr_backend_hint = NULL;

			if (bias_ops->abo_filter_req_fn != NULL)
				eligible = bias_ops->abo_filter_req_fn(&req);

			if (eligible) {
				if (bias_ops->abo_get_stream_id_fn != NULL)
					bias_stream_id = bias_ops->abo_get_stream_id_fn(&req);

				mutex_enter(&vd->vdev_alloc_bias_lock);
				bias_ctx = ab_context_lookup(vd, bias_ops, bias_key, bias_stream_id);

				alloc_bias_context_t *ctx_ptr = bias_ctx;
				alloc_bias_action_t action = BIAS_ACTION_FALLBACK;

				if (bias_ops->abo_advise_alloc_fn != NULL)
					action = bias_ops->abo_advise_alloc_fn(&ctx_ptr, &req);

				if (ctx_ptr == NULL && bias_ctx != NULL) {
					ab_context_remove(vd, bias_ctx);
					bias_ctx = NULL;
					action = BIAS_ACTION_FALLBACK;
				} else {
					bias_ctx = ctx_ptr;
				}

				if (action == BIAS_ACTION_ALLOC_FROM_HINT && bias_ctx != NULL &&
				    bias_ops->abo_get_hint_fn != NULL) {
					if (bias_ops->abo_get_hint_fn(bias_ctx, &bias_hint) == 0) {
						bias_hint_valid = B_TRUE;
					} else {
						action = BIAS_ACTION_FALLBACK;
					}
				} else if (action == BIAS_ACTION_CREATE_NEW_CONTEXT &&
				    bias_ctx == NULL) {
					create_ctx = B_TRUE;
				}

				mutex_exit(&vd->vdev_alloc_bias_lock);

				if (bias_hint_valid) {
					metaslab_t *hint_msp = bias_hint.abh_region_handle;
					if (hint_msp != NULL && hint_msp->ms_group->mg_vd == vd) {
						uint64_t hint_result = metaslab_alloc_from_hint(hint_msp,
						    bias_hint.abh_offset, asize, txg, obj_type);
						if (hint_result != -1ULL) {
							mc->mc_rotor = hint_msp->ms_group->mg_next;
							mc->mc_aliquot = 0;
							DVA_SET_VDEV(&dva[d], vd->vdev_id);
							DVA_SET_OFFSET(&dva[d], hint_result);
							DVA_SET_GANG(&dva[d], !!(flags & METASLAB_GANG_HEADER));
							DVA_SET_ASIZE(&dva[d], asize);

							if (bias_ops->abo_advance_fn != NULL) {
								mutex_enter(&vd->vdev_alloc_bias_lock);
								if (bias_ctx != NULL)
									bias_ops->abo_advance_fn(bias_ctx, asize);
								mutex_exit(&vd->vdev_alloc_bias_lock);
							}

							return (0);
						}
					}

					if (bias_ops->abo_get_alternative_hint_fn != NULL &&
					    bias_ctx != NULL) {
						mutex_enter(&vd->vdev_alloc_bias_lock);
						bias_ops->abo_get_alternative_hint_fn(bias_ctx, &bias_hint);
						mutex_exit(&vd->vdev_alloc_bias_lock);

						metaslab_t *alt_msp = bias_hint.abh_region_handle;
						if (alt_msp != NULL && alt_msp->ms_group->mg_vd == vd) {
							uint64_t alt_result = metaslab_alloc_from_hint(alt_msp,
							    bias_hint.abh_offset, asize, txg, obj_type);
							if (alt_result != -1ULL) {
								mc->mc_rotor = alt_msp->ms_group->mg_next;
								mc->mc_aliquot = 0;
								DVA_SET_VDEV(&dva[d], vd->vdev_id);
								DVA_SET_OFFSET(&dva[d], alt_result);
								DVA_SET_GANG(&dva[d], !!(flags & METASLAB_GANG_HEADER));
								DVA_SET_ASIZE(&dva[d], asize);

								if (bias_ops->abo_advance_fn != NULL) {
									mutex_enter(&vd->vdev_alloc_bias_lock);
									if (bias_ctx != NULL)
										bias_ops->abo_advance_fn(bias_ctx, asize);
									mutex_exit(&vd->vdev_alloc_bias_lock);
								}

								return (0);
							}
						}
					}

					if (bias_ctx != NULL) {
						mutex_enter(&vd->vdev_alloc_bias_lock);
						ab_context_remove(vd, bias_ctx);
						mutex_exit(&vd->vdev_alloc_bias_lock);
						bias_ctx = NULL;
					}
				}
			}
		}

		metaslab_t *selected_msp = NULL;
		offset = metaslab_group_alloc(mg, asize, txg, distance, dva, d,
		    obj_type, &selected_msp);
		if (offset != -1ULL) {
			/*
			 * If we've just selected this metaslab group,
			 * figure out whether the corresponding vdev is
			 * over- or under-used relative to the pool,
			 * and set an allocation bias to even it out.
			 */
			if (mc->mc_aliquot == 0) {
				vdev_stat_t *vs = &vd->vdev_stat;
				int64_t vu, cu;

				/*
				 * Determine percent used in units of 0..1024.
				 * (This is just to avoid floating point.)
				 */
				vu = (vs->vs_alloc << 10) / (vs->vs_space + 1);
				cu = (mc->mc_alloc << 10) / (mc->mc_space + 1);

				/*
				 * Bias by at most +/- 25% of the aliquot.
				 */
				mg->mg_bias = ((cu - vu) *
				    (int64_t)mg->mg_aliquot) / (1024 * 4);
			}

			if (atomic_add_64_nv(&mc->mc_aliquot, asize) >=
			    mg->mg_aliquot + mg->mg_bias) {
				mc->mc_rotor = mg->mg_next;
				mc->mc_aliquot = 0;
			}

			DVA_SET_VDEV(&dva[d], vd->vdev_id);
			DVA_SET_OFFSET(&dva[d], offset);
			DVA_SET_GANG(&dva[d], !!(flags & METASLAB_GANG_HEADER));
			DVA_SET_ASIZE(&dva[d], asize);

#ifdef ALLOC_DEBUG
		{
			hrtime_t now = gethrtime();
			uint64_t actual_offset = DVA_GET_OFFSET(&dva[d]);
			uint64_t hint_offset = (hintdva != NULL &&
			    DVA_IS_VALID(&hintdva[d])) ?
			    DVA_GET_OFFSET(&hintdva[d]) : (uint64_t)-1;
			metaslab_t *log_msp = selected_msp;
			if (log_msp == NULL && actual_offset != -1ULL) {
				uint64_t log_index = actual_offset >> vd->vdev_ms_shift;
				if (log_index < vd->vdev_ms_count)
					log_msp = vd->vdev_ms[log_index];
			}
			uint64_t metaslab_index = (log_msp != NULL) ?
			    (log_msp->ms_map.sm_start >> vd->vdev_ms_shift) : 0;

				printf("ALLOCDBG time=%" PRIu64 " vdev=%" PRIu64
				    " metaslab=%" PRIu64 " hint=0x%016" PRIx64
				    " actual=0x%016" PRIx64 " obj_type=%u\n",
				    (uint64_t)now,
				    vd->vdev_id,
				    metaslab_index,
				    hint_offset,
				    actual_offset,
				    (unsigned int)obj_type);
			}
#endif

			if (bias_ops != NULL && create_ctx && bias_key != 0 &&
			    selected_msp != NULL) {
				alloc_bias_context_t *new_ctx =
				    ab_context_alloc(bias_ops, bias_key, bias_stream_id);
				mutex_enter(&vd->vdev_alloc_bias_lock);
				ab_context_insert(vd, new_ctx);
				if (bias_ops->abo_new_context_fn != NULL) {
					bias_ops->abo_new_context_fn(new_ctx, bias_stream_id,
					    selected_msp, offset, asize);
				}
				if (bias_ops->abo_advance_fn != NULL)
					bias_ops->abo_advance_fn(new_ctx, asize);
				mutex_exit(&vd->vdev_alloc_bias_lock);
			}

			return (0);
		}
next:
		mc->mc_rotor = mg->mg_next;
		mc->mc_aliquot = 0;
	} while ((mg = mg->mg_next) != rotor);

	if (!all_zero) {
		dshift++;
		ASSERT(dshift < 64);
		goto top;
	}

	if (!allocatable && !zio_lock) {
		dshift = 3;
		zio_lock = B_TRUE;
		goto top;
	}

	bzero(&dva[d], sizeof (dva_t));

	return (ENOSPC);
}

/*
 * Free the block represented by DVA in the context of the specified
 * transaction group.
 */
static void
metaslab_free_dva(spa_t *spa, const dva_t *dva, uint64_t txg, boolean_t now)
{
	uint64_t vdev = DVA_GET_VDEV(dva);
	uint64_t offset = DVA_GET_OFFSET(dva);
	uint64_t size = DVA_GET_ASIZE(dva);
	vdev_t *vd;
	metaslab_t *msp;

	ASSERT(DVA_IS_VALID(dva));

	if (txg > spa_freeze_txg(spa))
		return;

	if ((vd = vdev_lookup_top(spa, vdev)) == NULL ||
	    (offset >> vd->vdev_ms_shift) >= vd->vdev_ms_count) {
		cmn_err(CE_WARN, "metaslab_free_dva(): bad DVA %llu:%llu",
		    (u_longlong_t)vdev, (u_longlong_t)offset);
		ASSERT(0);
		return;
	}

	msp = vd->vdev_ms[offset >> vd->vdev_ms_shift];

	if (DVA_GET_GANG(dva))
		size = vdev_psize_to_asize(vd, SPA_GANGBLOCKSIZE);

	mutex_enter(&msp->ms_lock);

	if (now) {
		space_map_remove(&msp->ms_allocmap[txg & TXG_MASK],
		    offset, size, METASLAB_ALLOC_UNKNOWN);
		space_map_free(&msp->ms_map, offset, size, METASLAB_ALLOC_UNKNOWN);
	} else {
		if (msp->ms_freemap[txg & TXG_MASK].sm_space == 0)
			vdev_dirty(vd, VDD_METASLAB, msp, txg);
		space_map_add(&msp->ms_freemap[txg & TXG_MASK], offset, size, METASLAB_ALLOC_UNKNOWN);
	}

	mutex_exit(&msp->ms_lock);
}

/*
 * Intent log support: upon opening the pool after a crash, notify the SPA
 * of blocks that the intent log has allocated for immediate write, but
 * which are still considered free by the SPA because the last transaction
 * group didn't commit yet.
 */
static int
metaslab_claim_dva(spa_t *spa, const dva_t *dva, uint64_t txg)
{
	uint64_t vdev = DVA_GET_VDEV(dva);
	uint64_t offset = DVA_GET_OFFSET(dva);
	uint64_t size = DVA_GET_ASIZE(dva);
	vdev_t *vd;
	metaslab_t *msp;
	int error = 0;

	ASSERT(DVA_IS_VALID(dva));

	if ((vd = vdev_lookup_top(spa, vdev)) == NULL ||
	    (offset >> vd->vdev_ms_shift) >= vd->vdev_ms_count)
		return (ENXIO);

	msp = vd->vdev_ms[offset >> vd->vdev_ms_shift];

	if (DVA_GET_GANG(dva))
		size = vdev_psize_to_asize(vd, SPA_GANGBLOCKSIZE);

	mutex_enter(&msp->ms_lock);

	if ((txg != 0 && spa_writeable(spa)) || !msp->ms_map.sm_loaded)
		error = metaslab_activate(msp, METASLAB_WEIGHT_SECONDARY, 0);

	if (error == 0 && !space_map_contains(&msp->ms_map, offset, size))
		error = ENOENT;

	if (error || txg == 0) {	/* txg == 0 indicates dry run */
		mutex_exit(&msp->ms_lock);
		return (error);
	}

	space_map_claim(&msp->ms_map, offset, size, METASLAB_ALLOC_UNKNOWN);

	if (spa_writeable(spa)) {	/* don't dirty if we're zdb(1M) */
		if (msp->ms_allocmap[txg & TXG_MASK].sm_space == 0)
			vdev_dirty(vd, VDD_METASLAB, msp, txg);
		space_map_add(&msp->ms_allocmap[txg & TXG_MASK], offset, size, METASLAB_ALLOC_UNKNOWN);
	}

	mutex_exit(&msp->ms_lock);

	return (0);
}

int
metaslab_alloc(spa_t *spa, metaslab_class_t *mc, uint64_t psize, blkptr_t *bp,
    int ndvas, uint64_t txg, blkptr_t *hintbp, int flags,
    dmu_object_type_t obj_type, zio_t *zio)
{
	dva_t *dva = bp->blk_dva;
	dva_t *hintdva = hintbp->blk_dva;
	int error = 0;

	ASSERT(bp->blk_birth == 0);
	ASSERT(BP_PHYSICAL_BIRTH(bp) == 0);

	spa_config_enter(spa, SCL_ALLOC, FTAG, RW_READER);

	if (mc->mc_rotor == NULL) {	/* no vdevs in this class */
		spa_config_exit(spa, SCL_ALLOC, FTAG);
		return (ENOSPC);
	}

	ASSERT(ndvas > 0 && ndvas <= spa_max_replication(spa));
	ASSERT(BP_GET_NDVAS(bp) == 0);
	ASSERT(hintbp == NULL || ndvas <= BP_GET_NDVAS(hintbp));

	for (int d = 0; d < ndvas; d++) {
		error = metaslab_alloc_dva(spa, mc, psize, dva, d, hintdva,
		    txg, flags, obj_type, zio);
		if (error) {
			for (d--; d >= 0; d--) {
				metaslab_free_dva(spa, &dva[d], txg, B_TRUE);
				bzero(&dva[d], sizeof (dva_t));
			}
			spa_config_exit(spa, SCL_ALLOC, FTAG);
			return (error);
		}
	}
	ASSERT(error == 0);
	ASSERT(BP_GET_NDVAS(bp) == ndvas);

	spa_config_exit(spa, SCL_ALLOC, FTAG);

	BP_SET_BIRTH(bp, txg, txg);

	return (0);
}

void
metaslab_free(spa_t *spa, const blkptr_t *bp, uint64_t txg, boolean_t now)
{
	const dva_t *dva = bp->blk_dva;
	int ndvas = BP_GET_NDVAS(bp);

	ASSERT(!BP_IS_HOLE(bp));
	ASSERT(!now || bp->blk_birth >= spa_syncing_txg(spa));

	spa_config_enter(spa, SCL_FREE, FTAG, RW_READER);

	for (int d = 0; d < ndvas; d++)
		metaslab_free_dva(spa, &dva[d], txg, now);

	spa_config_exit(spa, SCL_FREE, FTAG);
}

int
metaslab_claim(spa_t *spa, const blkptr_t *bp, uint64_t txg)
{
	const dva_t *dva = bp->blk_dva;
	int ndvas = BP_GET_NDVAS(bp);
	int error = 0;

	ASSERT(!BP_IS_HOLE(bp));

	if (txg != 0) {
		/*
		 * First do a dry run to make sure all DVAs are claimable,
		 * so we don't have to unwind from partial failures below.
		 */
		if ((error = metaslab_claim(spa, bp, 0)) != 0)
			return (error);
	}

	spa_config_enter(spa, SCL_ALLOC, FTAG, RW_READER);

	for (int d = 0; d < ndvas; d++)
		if ((error = metaslab_claim_dva(spa, &dva[d], txg)) != 0)
			break;

	spa_config_exit(spa, SCL_ALLOC, FTAG);

	ASSERT(error == 0 || txg == 0);

	return (error);
}
