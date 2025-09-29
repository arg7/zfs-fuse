#ifndef _SYS_ALLOC_BIAS_H
#define	_SYS_ALLOC_BIAS_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/avl.h>

/*
 * =============================================================================
 * ZFS Allocation Bias Framework
 *
 * This framework provides a mechanism to override the default ZFS metaslab
 * allocation strategy with custom, policy-driven "bias engines".
 * =============================================================================
 */

 typedef uint64_t htime_t; // high resolution time

// Forward declarations
struct vdev;
struct alloc_bias_hint;
typedef struct alloc_bias_ops alloc_bias_ops_t;
typedef struct alloc_bias_context alloc_bias_context_t;

/**
 * @brief The "Question" from the core allocator to the bias engine.
 */
typedef struct alloc_bias_req {
	void              *abr_io_req;
	uint64_t           abr_size;
	struct vdev       *abr_vdev;
	struct alloc_bias_hint *abr_backend_hint;
} alloc_bias_req_t;

/**
 * @brief The generic "Hint" provided by a bias engine.
 */
typedef struct alloc_bias_hint {
	void       *abh_region_handle; // Opaque handle to an allocation region (e.g., metaslab_t*).
	uint64_t    abh_offset;
	uint64_t    abh_flags;
} alloc_bias_hint_t;

/**
 * @brief The "Answer" or directive returned by a bias engine.
 */
typedef enum alloc_bias_action {
	BIAS_ACTION_FALLBACK,
	BIAS_ACTION_ALLOC_FROM_HINT,
	BIAS_ACTION_CREATE_NEW_CONTEXT
} alloc_bias_action_t;

/**
 * @brief The stateful context object for a single biased stream.
 */
typedef struct alloc_bias_context {
	avl_node_t          abc_node;
	alloc_bias_ops_t   *abc_ops;
	uint64_t            abc_primary_key;
	uint64_t            abc_stream_id;
	char                abc_private_data[];
} alloc_bias_context_t;

/**
 * @brief The "Engine": A stateless vtable defining a bias engine's behavior.
 */
typedef struct alloc_bias_ops {
	const char *abo_name;
	size_t      abo_private_ctx_size;

	/**
	 * A fast gatekeeper to determine if the engine is interested in this request.
	 * @param[in] req The allocation request details.
	 * @return B_TRUE if the request is eligible for biasing, B_FALSE otherwise.
	 */
	bool (*abo_filter_req_fn)(
	    const alloc_bias_req_t *req);

	/**
	 * Classifies an eligible request into an engine-defined stream ID.
	 * @param[in] req The allocation request details.
	 * @return A canonical stream ID defined by the engine.
	 */
	uint64_t (*abo_get_stream_id_fn)(
	    const alloc_bias_req_t *req);

	/**
	 * The primary decision-making function.
	 * @param[in,out] abc_p Pointer to the caller's context variable. The engine
	 *                      can modify this to signal the old context is invalid.
	 * @param[in]     req   The allocation request details.
	 * @return An alloc_bias_action_t directive.
	 */
	alloc_bias_action_t (*abo_advise_alloc_fn)(
	    alloc_bias_context_t **abc_p,
	    const alloc_bias_req_t *req);

	/**
	 * Called after a new segment is found to initialize a new context.
	 */
	void (*abo_new_context_fn)(
	    alloc_bias_context_t *abc,
	    uint64_t stream_id,
	    void    *abh_region_handle,
	    uint64_t segment_start,
	    uint64_t segment_size);

	/**
	 * Fills a generic hint structure from the context's private state.
	 */
	int (*abo_get_hint_fn)(
	    alloc_bias_context_t *abc,
	    alloc_bias_hint_t *hint_out);

	/**
	 * Called by the core allocator after a successful allocation using a hint.
	 */
	void (*abo_advance_fn)(
	    alloc_bias_context_t *abc,
	    uint64_t allocated_size);

	/**
	 * Called by the framework's cleanup routine to check for stale contexts.
	 */
	bool (*abo_is_stale_fn)(
	    alloc_bias_context_t *abc,
	    htime_t now);

	/**
	 * Checks if a standard ZFS hint conflicts with this engine's reservations.
	 */
	alloc_bias_context_t *(*abo_find_conflicting_context_fn)(
	    const void *hint_handle);

	/**
	 * Called when a conflict is found to ask for a non-conflicting hint.
	 */
	void (*abo_get_alternative_hint_fn)(
	    alloc_bias_context_t *conflicting_abc,
	    alloc_bias_hint_t *hint_out);

} alloc_bias_ops_t;


/*
 * =============================================================================
 * Framework Public API
 * =============================================================================
 */

int ab_register_engine(alloc_bias_ops_t *ops);
void ab_deregister_engine(alloc_bias_ops_t *ops);
alloc_bias_ops_t *ab_find_engine_by_name(const char *name);

void ab_vdev_init(struct vdev *vd);
void ab_vdev_fini(struct vdev *vd);

alloc_bias_context_t *ab_context_alloc(alloc_bias_ops_t *ops,
    uint64_t primary_key, uint64_t stream_id);
alloc_bias_context_t *ab_context_lookup(struct vdev *vd,
    alloc_bias_ops_t *ops, uint64_t primary_key, uint64_t stream_id);
void ab_context_insert(struct vdev *vd, alloc_bias_context_t *ctx);
void ab_context_remove(struct vdev *vd, alloc_bias_context_t *ctx);

#endif /* _SYS_ALLOC_BIAS_H */
