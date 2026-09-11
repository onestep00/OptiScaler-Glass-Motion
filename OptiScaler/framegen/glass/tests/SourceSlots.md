# Direct source-slot handoff

- Created: 2026-09-11
- Updated: 2026-09-11
- Status: implemented; standalone CPU checks passed; engine adapter incomplete
- Deployment: none; no motion or FG input changes
- Deprecated: no
- Scope: bounded CPU correspondence within one proven producer/consumer domain

`GeometrySourceSlots.h` stores source instance identities at transform-slot indices.
Capacity is selected by its owner at compile time. Publication and lookup perform
no allocation, table search or GPU operation. Starting a new domain changes an
epoch instead of clearing entries; only 64-bit epoch wrap clears the storage.
Storage must be owned outside a draw callback's stack in production.

The caller must supply the actual view, frame, owner lifetime generation and
source-array identity generation. None is inferred from an address, matching
transform, unchanged array header or draw order. Array content updates need not
change element identity; the engine adapter must establish that distinction.
The class is not a lifetime observer or motion producer.

Publication must finish before sealing and consumer lookup. Conflicting or
invalid source entries poison the individual slot for that domain. Later matching
publication cannot undo a conflict. Every begin creates a new ticket even when
view/frame values are repeated. Previous tickets, absent slots, out-of-range
slots, incomplete domains and missing view/frame are rejected. Access must be
externally ordered; concurrent producer/consumer access is unsupported. A cache
cannot be recycled while consumers still use its ticket.

`SourceSlots.cpp` checks reordered instances, stale same-frame submissions,
sticky ambiguity, invalid entries, owner/array generation passthrough and range
rejection. MSVC C++20 `/O2 /W4 /WX` build and executable passed on 2026-09-11.
These are owned CPU fixtures, not engine lifetime or GPU/FG evidence.

Before runtime use, connect actual producer view/submission provenance and
registered owner lifetime plus verified source-array identity to this handoff.
Do not use the prior offline same-frame range matches alone to populate admitted
vertex history. No production call site has been added yet.
