### 📜 RFC: A Generic Allocation Bias Framework and Streaming Engine for ZFS

**Status:** Proposed (Revised Architecture)
**Author:** AR & Gemini
**OpenZFS-Version:** 2.2+
**Date:** 2025-09-25

#### 1. Problem Statement
(This remains unchanged from your original, as it is the core motivation.)
In large-scale archival workloads, ZFS performance is often dominated by disk head seeks. The default allocator, optimized for general-purpose workloads, scatters blocks and destroys spatial locality for concurrent, append-heavy writers, leading to performance far below the theoretical maximum of the hardware.

#### 2. Goals
(Unchanged)
Design an allocation system that reduces disk seeks by ensuring contiguous layout, isolates concurrent writers, separates data and metadata streams, and survives fragmentation gracefully.

#### 3. Design Overview: A Two-Layer Architecture
This proposal introduces a two-layer architecture to solve the problem in a generic and extensible way:

*   **The Allocation Bias Framework:** A new, generic, and policy-free framework integrated at the `vdev` level. Its purpose is to allow pluggable "bias engines" to advise the core ZFS allocator. It manages the lifecycle of stateful "context" objects but knows nothing about their internal logic. This framework is the platform for future allocation innovations.
*   **The Streaming Bias Engine:** The first implementation of a bias engine that uses this framework. This engine contains all the policy and logic to solve the original problem. It provides cursor-guided, contiguous allocation for concurrent writers, keyed by PGID, UID, or GID, and intelligently separates data and metadata streams.

#### 4. The Allocation Bias Framework
This generic framework is defined in a new header, `sys/zfs/alloc_bias.h`.

*   **Core Components:**
    *   `alloc_bias_ops`: A stateless "vtable" struct that defines the interface for any bias engine.
    *   `alloc_bias_context`: A stateful object, managed by the framework, that holds an engine's private data for a single allocation stream.
    *   `alloc_bias_req_t`: A "question" struct that packages an allocation request for an engine to evaluate.
    *   `alloc_bias_hint_t`: A generic "answer" struct that an engine uses to provide its allocation hint.
*   **Integration:**
    *   **State:** The framework's state (a tree of active `alloc_bias_context` objects) is anchored in the `vdev_t` struct, ensuring per-vdev isolation and resource management. It is protected by the existing `vdev_metaslab_lock`.
    *   **Hook Point:** The framework hooks into `metaslab_alloc()`. It acts as a dispatcher, consulting the active engine for a given dataset before falling back to the default ZFS allocator.
    *   **Plumbing:** The `zio_t` struct is extended to carry the necessary context (UID, GID, etc.) and policy information down to the allocator.

#### 5. The Streaming Bias Engine
This is the first "plugin" for the framework, providing the features from the original RFC.

*   **Engine Logic:** It implements the `alloc_bias_ops` interface.
*   **Filtering & Classification (`abo_filter_req_fn`, `abo_get_stream_id_fn`):** The engine first filters requests it is interested in (e.g., user data writes). It then classifies them into canonical streams, primarily separating `ZIO_TYPE_DATA` from all metadata types (`ZIO_TYPE_DNODE`, `ZIO_TYPE_INDIRECT`, etc.) into two distinct streams.
*   **Keying Policy:** The engine can be configured via a dataset property to key its contexts on a per-Process Group ID (`pgid`), per-User ID (`uid`), or per-Group ID (`gid`) basis.
*   **Resilience ("Skip-and-Continue"):** The engine is tolerant of fragmentation. If a small, rogue allocation punctures its soft-reserved segment, it will intelligently "hop over" the puncture and continue its stream, rather than immediately invalidating its context.
*   **Prevention ("Hint Manipulation"):** The engine also implements ops to detect when the default ZFS allocator's hint (`hintbp`) conflicts with one of its active segments. If a conflict is detected, the engine provides an alternative hint to gently steer the default allocator away, making context invalidation a rare event.

#### 6. Features & Tunables
*   **Framework Property:**
    *   `allocation:strategy = "default" | "streaming"`: Selects the active bias engine for a dataset.
*   **Streaming Engine Properties:**
    *   `streaming:bias_key = "pgid" | "uid" | "gid"`: (Default: `pgid`) Defines the key for stream isolation.
    *   `streaming:max_contexts = 64`: (Default: 64) The maximum number of concurrent streams the engine will manage on a single vdev before falling back.
    *   `streaming:timeout_data = 300`: (Default: 300s) Inactivity timeout for data stream contexts.
    *   `streaming:timeout_metadata = 600`: (Default: 600s) Inactivity timeout for metadata stream contexts.

#### 7. Future Work
The Allocation Bias Framework is explicitly designed for extensibility. Future work could include developing new engines for different workloads, such as:
*   A locality engine that attempts to keep all blocks for a single file physically contiguous.
*   An experimental engine that uses userspace hooks (via FUSE or `ioctl`) to allow ML models to drive allocation decisions.
