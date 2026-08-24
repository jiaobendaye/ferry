## 1. Regression Coverage

- [x] 1.1 Add a closed-loop integration test with server `cap_bytes` smaller than client `chunk_size`, and verify it reproduces a hash mismatch before the fix.

## 2. Client Range Accumulation

- [x] 2.1 Track the next unwritten offset within each claimed logical chunk and request only its remaining suffix.
- [x] 2.2 Advance byte progress after each successful subrange write, but persist bitmap completion and claim a new chunk only after the logical chunk is fully written.

## 3. Verification

- [x] 3.1 Run the focused regression test and the complete unit and integration test suites.
- [x] 3.2 Re-run the real-binary reproduction with `server cap_bytes < client chunk_size` and verify matching output hashes.
