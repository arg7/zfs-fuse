#ifndef _SYS_ALLOC_BIAS_BE_H
#define	_SYS_ALLOC_BIAS_BE_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/dmu.h>

// =========================================================================
// Backend Interface
// =========================================================================

struct vdev;

/**
 * @brief Describes a single contiguous free segment.
 */
typedef struct vab_free_segment {
    uint64_t vfs_offset; // Starting offset of the free segment
    uint64_t vfs_size;   // Size of the free segment
} vab_free_segment_t;

/**
 * @brief Summary statistics describing a region (e.g., a metaslab).
 */
typedef struct vab_region_stats {
    uint64_t vrs_start;
    uint64_t vrs_size;
    uint64_t vrs_allocated;
    uint64_t vrs_free_space;
    uint64_t vrs_max_segment;
    boolean_t vrs_loaded;
    boolean_t vrs_active;
} vab_region_stats_t;

/**
 * @brief Defines the iteration order for free segments.
 */
typedef enum vab_iter_order {
    // Iterate through free segments in ascending order of their offset.
    // This is useful for finding the "next available" space.
    VAB_ITER_ORDER_OFFSET,

    // Iterate through free segments in descending order of their size.
    // This is the most useful for finding the largest available chunks first.
    VAB_ITER_ORDER_SIZE
} vab_iter_order_t;

// Opaque forward declaration for the iterator handle
typedef struct vab_iter vab_iter_t;

/*
 * Updated vdev_alloc_backend_ops_t with iterator functions.
 * (Includes previous functions for context)
 */
typedef struct vdev_alloc_backend_ops {
    void *(*vab_first_region)(struct vdev *vd);
    void *(*vab_next_region)(void *region_handle);
    int (*vab_get_region_stats)(void *region_handle,
        vab_region_stats_t *stats_out);
    int (*vab_alloc)(void *region_handle, uint64_t offset, uint64_t size,
        dmu_object_type_t obj_type, uint64_t txg);
    boolean_t (*vab_is_free)(void *region_handle, uint64_t offset,
        uint64_t size);

    /**
     * @brief Creates an iterator for the free segments within a region.
     *
     * The backend allocates and initializes an iterator object. The caller is
     * responsible for destroying it via vab_iter_destroy().
     *
     * @param[in]  region_handle The handle of the region to iterate over.
     * @param[in]  order         The desired ordering of segments (by offset or size).
     * @param[out] iter_out      On success, holds the handle to the new iterator.
     * @return 0 on success, error code on failure.
     */
    int (*vab_iter_create)(void *region_handle, vab_iter_order_t order,
        vab_iter_t **iter_out);

    /**
     * @brief Retrieves the next free segment from an iterator.
     *
     * @param[in]  iter        The iterator handle.
     * @param[out] segment_out The structure to fill with segment data.
     * @return B_TRUE if a segment was found, B_FALSE if the iteration is complete.
     */
    bool (*vab_iter_next)(vab_iter_t *iter, vab_free_segment_t *segment_out);

    /**
     * @brief Destroys an iterator and releases its resources.
     *
     * @param[in] iter The iterator handle to destroy.
     */
    void (*vab_iter_destroy)(vab_iter_t *iter);

} vdev_alloc_backend_ops_t;

#endif /* _SYS_ALLOC_BIAS_BE_H */
