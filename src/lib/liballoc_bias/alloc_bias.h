#ifndef _SYS_AB_STREAMING_H
#define	_SYS_AB_STREAMING_H

#include <sys/spa.h>
#include <sys/space_map.h>
#include <sys/txg.h>
#include <sys/zio.h>
#include <sys/avl.h>
#include <sys/alloc_bias.h>

#define AB_STREAMING (1)


typedef struct abc_streaming_data {

    uid_t       stc_uid;
    gid_t       stc_gid;
    pid_t       stc_gpid;

    metaslab_t* stc_metaslab;
    uint64_t    stc_segment_start;  // ← original segment start (new)
    uint64_t    stc_segment_end;    // ← original segment end (unchanged)
    uint64_t    stc_cursor;         // ← current write head
    uint64_t    stc_chunk_size;     // ← max this PID can consume (capped)
    uint64_t    stc_last_used;
};

#endif
