#ifndef _SYS_ALLOC_BIAS_STREAMING_H
#define	_SYS_ALLOC_BIAS_STREAMING_H

#include <stdint.h>
#include <sys/spa.h>

typedef struct streaming_bias_private {
    stream_type_t sbp_stream_type; // DATA or METADATA
    metaslab_t *sbp_metaslab; // Reserved metaslab
    uint64_t sbp_segment_start; // Soft reservation start
    uint64_t sbp_segment_end; // Soft reservation end (mutable)
    uint64_t sbp_cursor; // Current allocation offset
    uint64_t sbp_chunk_size; // Chunk size
    uint64_t sbp_last_used; // Last activity timestamp
} streaming_bias_private_t;

#endif	_SYS_ALLOC_BIAS_STREAMING_H