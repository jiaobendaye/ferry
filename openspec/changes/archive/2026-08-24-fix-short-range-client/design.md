## Context

The client plans fixed logical chunks for parallel claiming, retry, and resume bookkeeping. A worker currently assumes one HTTP `206` completes one logical chunk, but HTTP Range servers may legally return a shorter interval, including ferry-server when `cap_bytes` is smaller than the requested range. The received prefix is written correctly, while the unreceived suffix remains a sparse-file hole and the bitmap incorrectly records the whole chunk as complete.

## Goals / Non-Goals

**Goals:**

- Accumulate one or more valid `206` responses into a logical chunk.
- Keep each follow-up request within the original logical chunk boundary.
- Update byte progress per successful write and chunk progress only after the full logical chunk is durable.
- Preserve current retry, resume, concurrency, and bounded-memory behavior.

**Non-Goals:**

- Changing server Range or cap semantics.
- Persisting sub-chunk progress across process restarts.
- Changing chunk planning, bitmap format, CLI options, or single-stream fallback.

## Decisions

### Track the next unwritten offset on each worker job

Each `ChunkJob` will retain a cursor for the next byte needed in its currently claimed logical chunk. Request construction will use the cursor as the Range start and the logical chunk end as the Range end. This keeps existing fixed-chunk ownership and avoids introducing a second scheduler for server-sized fragments.

Alternative: resize the client chunk size after probing the server. The server does not advertise its cap, and generic servers may choose different response lengths dynamically, so this cannot guarantee correctness.

### Complete a chunk only after all subranges are written

After a successful `pwrite`, the cursor and byte progress advance by the actual response length. If bytes remain, the same worker immediately requests the suffix. Only the final write marks the bitmap and increments completed chunks before the worker claims another chunk.

Partial sub-chunk progress remains intentionally ephemeral. If interrupted, the logical chunk is absent from the bitmap and is downloaded again from its start, preserving the existing resume format and safety model.

### Reset transient retry attempts after forward progress

A successfully written short response proves forward progress. The retry attempt counter resets before requesting the remaining suffix so a large logical chunk split into many server-capped responses does not accumulate unrelated failures across successful transfers.

## Risks / Trade-offs

- [A server returns zero bytes in a successful `206`] -> Existing interval/body validation rejects an empty or malformed interval; no cursor advance loop is permitted.
- [Interruption after a partial subrange causes redundant transfer] -> The whole logical chunk is safely re-downloaded, matching existing bitmap granularity.
- [More requests when the server cap is small] -> This is required by the server contract; keep-alive is retained to limit connection overhead.
- [Progress accounting could double-count retries] -> Advance byte progress only after a successful `pwrite`, and retry from the unchanged cursor after failures.
