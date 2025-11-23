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
 * Copyright 2025 Antigravity.  All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/zfs_context.h>
#include <sys/alloc_bias.h>
#include <sys/vdev_impl.h>
#include <sys/metaslab_impl.h>
#include <sys/zio.h>

/*
 * Sequential Bias Engine
 *
 * This engine attempts to allocate blocks sequentially on a per-vdev basis.
 * It maintains a cursor (offset) for each vdev and tries to allocate
 * immediately after the last successful allocation.
 */

typedef struct seq_bias_private {
	uint64_t	sb_cursor;	/* Last allocated offset */
} seq_bias_private_t;

static bool
seq_filter_req(const alloc_bias_req_t *req)
{
	/* Handle all requests for now */
	return (B_TRUE);
}

static uint64_t
seq_get_stream_id(const alloc_bias_req_t *req)
{
	/*
	 * For this simple sequential engine, we treat all allocations
	 * on a vdev as a single stream (stream_id = 0).
	 * A more complex version could have per-dataset streams.
	 */
	return (0);
}

static void
seq_new_context(alloc_bias_context_t *abc, uint64_t stream_id,
    void *abh_region_handle, uint64_t segment_start, uint64_t segment_size)
{
	seq_bias_private_t *priv = (seq_bias_private_t *)abc->abc_private_data;

	/* Initialize cursor to the start of the segment */
	priv->sb_cursor = segment_start;
}

static alloc_bias_action_t
seq_advise_alloc(alloc_bias_context_t **abc_p, const alloc_bias_req_t *req)
{
	alloc_bias_context_t *abc = *abc_p;
	seq_bias_private_t *priv;

	if (abc == NULL) {
		/* No context exists, request a new one */
		return (BIAS_ACTION_CREATE_NEW_CONTEXT);
	}

	priv = (seq_bias_private_t *)abc->abc_private_data;

	/*
	 * In a real implementation, we would check if the cursor is still
	 * valid and points to free space. For this prototype, we rely on
	 * the allocator to fail if the hint is invalid, which will trigger
	 * a fallback or a new context creation.
	 */

	return (BIAS_ACTION_ALLOC_FROM_HINT);
}

static int
seq_get_hint(alloc_bias_context_t *abc, alloc_bias_hint_t *hint_out)
{
	seq_bias_private_t *priv = (seq_bias_private_t *)abc->abc_private_data;

	/*
	 * We need a handle to the metaslab.
	 * In the current API, the context doesn't explicitly store the metaslab
	 * handle, but we need it to construct the hint.
	 *
	 * Limitation: The current context structure assumes the engine knows
	 * the region handle. We might need to store it in our private data
	 * or look it up.
	 *
	 * For this prototype, we'll assume we can't easily get the metaslab
	 * handle back without storing it. Let's update private data to store it.
	 */
	
	/* 
	 * WAIT: The API design in ab_spec.md says abo_new_context_fn receives
	 * abh_region_handle. We should store it.
	 * Redefining private struct to include region handle.
	 */
	return (1); // Error, need to update struct first
}

/* Redefining struct with region handle */
typedef struct seq_bias_private_v2 {
	uint64_t	sb_cursor;
	void		*sb_region_handle; /* metaslab_t * */
} seq_bias_private_v2_t;

static void
seq_new_context_v2(alloc_bias_context_t *abc, uint64_t stream_id,
    void *abh_region_handle, uint64_t segment_start, uint64_t segment_size)
{
	seq_bias_private_v2_t *priv = (seq_bias_private_v2_t *)abc->abc_private_data;

	priv->sb_cursor = segment_start;
	priv->sb_region_handle = abh_region_handle;
}

static int
seq_get_hint_v2(alloc_bias_context_t *abc, alloc_bias_hint_t *hint_out)
{
	seq_bias_private_v2_t *priv = (seq_bias_private_v2_t *)abc->abc_private_data;

	hint_out->abh_region_handle = priv->sb_region_handle;
	hint_out->abh_offset = priv->sb_cursor;

	return (0);
}

static void
seq_advance(alloc_bias_context_t *abc, uint64_t allocated_size)
{
	seq_bias_private_v2_t *priv = (seq_bias_private_v2_t *)abc->abc_private_data;

	priv->sb_cursor += allocated_size;
}

static bool
seq_is_stale(alloc_bias_context_t *abc, htime_t now)
{
	/* Never stale for this simple test */
	return (B_FALSE);
}

static alloc_bias_ops_t seq_bias_ops = {
	.abo_name = "sequential",
	.abo_private_ctx_size = sizeof (seq_bias_private_v2_t),
	.abo_filter_req_fn = seq_filter_req,
	.abo_get_stream_id_fn = seq_get_stream_id,
	.abo_advise_alloc_fn = seq_advise_alloc,
	.abo_new_context_fn = seq_new_context_v2,
	.abo_get_hint_fn = seq_get_hint_v2,
	.abo_advance_fn = seq_advance,
	.abo_is_stale_fn = seq_is_stale,
	.abo_find_conflicting_context_fn = NULL,
	.abo_get_alternative_hint_fn = NULL
};

void
alloc_bias_sequential_init(void)
{
	ab_register_engine(&seq_bias_ops);
}

void
alloc_bias_sequential_fini(void)
{
	ab_deregister_engine("sequential");
}
