## Why

An RFC-compliant Range server may return a shorter interval than requested. The ferry client currently writes that interval and marks the entire logical chunk complete, so `server cap_bytes < client chunk_size` can produce a corrupt sparse output while the client exits successfully.

## What Changes

- Track progress within each logical download chunk.
- Continue requesting the unreceived suffix after a valid short `206` response.
- Mark the completion bitmap only after every byte in the logical chunk has been written.
- Add closed-loop regression coverage where the server response cap is smaller than the client chunk size.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `resumable-download-client`: Clarify that valid short Range responses must be accumulated until the requested logical chunk is complete.

## Impact

- Client Range worker state and request construction in `client/download/engine.cc`.
- Client/server closed-loop integration tests.
- No CLI, wire-protocol, dependency, or server behavior changes.
