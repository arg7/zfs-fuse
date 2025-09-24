graph TD
    subgraph "Write I/O Path"
        A((PGID->write)) --> B{Is `allocation=archive_concurrent` set?};
        B -- No --> C[Use Default Allocator];
        B -- Yes --> D{Context Exists for PGID?};
        D -- No --> ALG1_START(Start Algorithm I);
        D -- Yes --> ALG2_START(Start Algorithm II);
    end

    subgraph "Algorithm I: New Stream Activation"
        ALG1_START --> E{Context Array Full?};
        E -- Yes --> C;
        E -- No --> F(Tier 1: Find Untouched Metaslab);
        F --> G{Found?};
        G -- Yes --> SUCCESS_CREATE[Create New Context];
        G -- No --> H(Tier 2: Find Sharable Context);
        H --> I{Found?};
        I -- Yes --> J[Split Reservation & Create Context];
        I -- No --> C;
    end

    subgraph "Algorithm II: Allocate in Active Stream"
        ALG2_START --> K{Chunk has space for write?};
        K -- No --> L{Attempt to Chain to New Chunk};
        L -- No (Reservation Exhausted) --> Invalidate;
        L -- Yes (Chained) --> M[Get Hint from Cursor];
        K -- Yes --> M;

        M --> N[Attempt Allocation at Hint];
        N --> O{Was `actual_offset == hint_offset`?};
        O -- Yes (Success) --> P[Advance Cursor & Timestamp];
        O -- No (Conflict) --> Invalidate[Invalidate Context];
    end

    subgraph "Finalization"
        C --> R{ok?};
        R -- No --> FAIL[Fail I/O];
        R -- Yes --> SUCCESS[Success];
        
        SUCCESS_CREATE --> M;
        J --> M;
        
        P --> SUCCESS;
        Invalidate --> SUCCESS;
    end

    FAIL --> X((End));
    SUCCESS --> X((End));
