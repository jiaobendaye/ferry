## Why

Byte-valued server configuration currently requires precomputed decimal literals, while byte-valued client options also require callers to spell common MiB values as raw byte counts. Operators should be able to use explicit binary byte units for server configuration, and client users should be able to enter chunk and fallback limits directly as integer MiB counts.

## What Changes

- Accept optional `B`, `KiB`, `MiB`, `GiB`, and `TiB` suffixes for `cap_bytes` and `rate_bytes_per_sec`.
- Allow optional whitespace between the integer magnitude and suffix.
- Keep unsuffixed decimal integers interpreted as bytes.
- Reject unknown or incorrectly cased suffixes, negative magnitudes, multiplication overflow, and results outside each key's existing range.
- Document that a suffixed `rate_bytes_per_sec` value is a byte quantity per second, for example `10MiB` means 10 MiB/s.
- Interpret `--chunk-size` and `--single-stream-limit` values as integer MiB counts, converting them to bytes internally with overflow checks.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `range-file-serving`: Allow `cap_bytes` to use an explicit binary byte-unit suffix.
- `bandwidth-rate-limiting`: Allow `rate_bytes_per_sec` to use an explicit binary byte-unit suffix.
- `resumable-download-client`: Express the client's chunk size and single-stream limit options in MiB instead of raw bytes.

## Impact

- Server configuration and client CLI parsing, plus their unit tests.
- English and Chinese configuration and client documentation.
- No wire-protocol, runtime data-path, dependency, or default-value changes.
