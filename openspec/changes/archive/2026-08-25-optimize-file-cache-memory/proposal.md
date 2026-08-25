## Why

Large sequential downloads currently populate Linux page cache with the complete
read working set while the `pread` response path also holds one anonymous buffer
per in-flight response. A controlled 500 MiB download showed that anonymous
buffers drain correctly but the full 500 MiB remains cached, so operators need a
safe way to distinguish reclaimable cache from leaks and, for one-shot file
workloads, reduce cache retention without global `drop_caches` operations.

## What Changes

- Add a configurable file-cache policy with a compatibility-preserving `normal`
  default, a non-binding `noreuse` hint, and an opt-in `drop_after_read` policy
  for clean pages copied into `pread` response buffers.
- Keep response correctness and buffer lifetime unchanged: cache advice occurs
  only after a successful read and before closing the file descriptor, while the
  anonymous response buffer remains owned until HTTP completion.
- Extend server statistics with best-effort cgroup-v2 memory gauges for anonymous
  memory, file-backed cache, and socket memory, including an explicit
  unavailable state rather than misleading zeroes.
- Report cache-advice attempts, advised bytes, and failures so operators can
  prove that the configured policy is active without inferring from RSS alone.
- Document a bounded-memory deployment recipe combining `cap_bytes`,
  `max_inflight`, cgroup `MemoryHigh`/`MemoryMax`, and the cache policy.
- Add regression and stress coverage for policy parsing, advice placement,
  response integrity, memory attribution, cache residency, and repeated-reader
  performance.
- Publish the measured baseline, expected gains, trade-offs, rollout gates, and
  acceptance thresholds in `optimization-report.md`.

No existing default behavior changes and there are no breaking changes.

## Capabilities

### New Capabilities

- `file-cache-management`: Configurable, bounded-scope page-cache advice for
  large file responses, with safe defaults, deterministic configuration
  validation, and documented deployment controls.

### Modified Capabilities

- `server-observability`: Add cgroup-v2 anonymous/file/socket memory gauges to
  periodic and on-demand statistics without conflating logical mmap bytes with
  physical page-cache residency.

## Impact

- Server configuration parsing and startup logging.
- The asynchronous `pread` body path and its file-descriptor lifecycle; the
  experimental `mmap` path remains unchanged and explicitly unsupported by the
  drop-after-read policy.
- Stats snapshots/formatting and Linux cgroup-v2 discovery/read logic.
- Unit, integration, and opt-in stress tests plus operator documentation.
- No protocol, client CLI, response-body, or on-disk file-format changes.
