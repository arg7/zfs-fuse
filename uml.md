## UML diagrams


```mermaid
graph TD
    A((PGID->write)) --> B{Is `archive_concurrent` enabled?};
    B -- No --> C[Default Allocator];
    B -- Yes --> D{Active Context Exists?};
    
    D -- No --> ALG1(Execute Algorithm I: New Stream Activation);
    ALG1 --> H{Context Created?};
    H -- No --> C;
    H -- Yes --> F[Get Context Hint];
    
    D -- Yes --> ALG2_PRE(Pre-Allocation Checks);
    ALG2_PRE --> K{Chunk has space?};
    K -- Yes --> F;
    K -- No --> L{Can Chain to New Chunk?};
    
    L -- Yes --> Adjust_Chain[Adjust Context for New Chunk];
    Adjust_Chain --> F;
    
    L -- No --> Invalidate_and_Retry[Invalidate Context & Retry Allocation];
    Invalidate_and_Retry --> ALG1;

    F --> M[Attempt Allocation at Hint];
    M --> N{Hint == Actual Block?};
    
    N -- Yes (Success) --> T[Advance Cursor & Timestamp];
    T --> SUCCESS[Success];
    
    N -- No (Conflict) --> Invalidate_And_Check[Invalidate Context & Check Status];
    Invalidate_And_Check --> O{Was a Block Allocated *Anywhere*?};
    O -- Yes --> SUCCESS;
    O -- No --> C;

    C --> R{ok?};
    R -- No --> FAIL[Fail I/O];
    R -- Yes --> SUCCESS;

    FAIL --> X((End));
    SUCCESS --> X((End));
```
