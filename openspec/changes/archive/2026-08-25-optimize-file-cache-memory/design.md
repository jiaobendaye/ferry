## Context

The stable server path allocates one buffer for the decided response length,
schedules `pread`, attaches that buffer to Workflow with nocopy semantics, and
frees it only from the unified HTTP completion callback. Buffered file I/O also
populates Linux page cache. The response buffer is therefore bounded by
`inflight × cap_bytes`, while the reclaimable file-cache working set may grow to
the unique bytes read and remain after requests or the server finish.

The controlled measurements and target thresholds are recorded in
`optimization-report.md`. The implementation must preserve the current default,
must not invalidate a buffer still referenced by Workflow, and must treat Linux
cache advice as best effort rather than as a hard residency guarantee.

## Goals / Non-Goals

**Goals:**

- Make one-shot large-file workloads able to avoid retaining the complete read
  working set in page cache.
- Preserve byte-for-byte HTTP response behavior and the existing `normal`
  performance profile by default.
- Distinguish anonymous response memory, cgroup file memory, socket memory, and
  logical mmap mappings in operator-visible statistics.
- Provide measurable rollout, rollback, and capacity-planning guidance.

**Non-Goals:**

- Implement a per-file or global hard page-cache quota; Linux does not provide
  one through `posix_fadvise`.
- Invoke `/proc/sys/vm/drop_caches`, tune host-wide VM sysctls, or manage cgroups
  from inside the server.
- Add `O_DIRECT`, `sendfile`, streaming response bodies, or a response-buffer
  pool in this change.
- Change the experimental mmap body path or attribute cgroup file memory to an
  individual served file.

## Decisions

### D1: Add an explicit three-state cache policy

`page_cache_policy` accepts `normal`, `noreuse`, or `drop_after_read` and defaults
to `normal`.

- `normal` performs no new advice and preserves current behavior.
- `noreuse` issues `POSIX_FADV_NOREUSE` for the decided range before the read.
  It is a low-risk replacement-policy hint and can be a no-op on older kernels.
- `drop_after_read` issues `POSIX_FADV_DONTNEED` only after a successful `pread`
  copied the bytes into the anonymous response buffer and before the fd closes.

An enum is carried in `ServerConfig` and `FileBodySpec`; string conversion is
used by the startup line. Unknown values fail startup with the key and allowed
values in the error.

Alternatives rejected: making drop-after-read the default would regress repeated
downloads; using a boolean would not distinguish a gentle hint from eager drop.

### D2: Advise only fully covered pages and never change response outcome

The advisor computes the largest page-aligned interval fully contained in the
successful `[offset, offset + bytes_read)` result. Leading and trailing partial
pages are left alone because Linux may ignore partial-page `DONTNEED`, and
expanding the interval could evict adjacent data not read by this request.

Advice is isolated behind a small injectable function/object so unit tests can
assert policy, timing, offset, length, and error handling without depending on
host cache state. Advice failure increments counters but does not replace a
successful file response with 5xx. Read failures do not issue drop advice.

The response buffer remains owned by `PreadBody` until HTTP completion; page
cache advice never returns or reuses that anonymous buffer.

Alternatives rejected: advising at HTTP completion would require retaining or
reopening the fd; advising before `pread` with `DONTNEED` cannot remove pages
populated by the read.

### D3: Reject non-normal policies with mmap mode

Startup fails when `file_body_mode = mmap` is combined with `noreuse` or
`drop_after_read`. An mmap response can fault pages after setup and its fd is
already closed, so claiming equivalent post-read behavior would be misleading.
The operator can select stable `pread` or return the cache policy to `normal`.

### D4: Expose policy activity as cumulative counters

Stats add cumulative `cache_advice_calls`, `cache_advice_bytes`, and
`cache_advice_errors`. A successful advisory syscall increments calls and bytes;
a failed syscall increments calls and errors but not bytes. `normal` leaves all
three at zero. These counters identify attempted policy activity; they do not
claim that the kernel evicted the same number of bytes.

### D5: Sample cgroup-v2 memory only when emitting stats

A Linux cgroup memory reader resolves the process's unified cgroup from
`/proc/self/cgroup`, resolves the cgroup2 mount from `/proc/self/mountinfo`, and
parses `memory.stat`. It returns `anon`, `file`, and `sock` in bytes. Sampling
occurs only for periodic or SIGUSR1 output, not on the request hot path.

The stats line uses `mem_anon`, `mem_file`, and `mem_sock`. Each is `-1` when
cgroup v2, the controller, a field, or a readable path is unavailable. This is
preferable to zero, which means a real measured zero. Parsers use injected text
or paths in unit tests. Existing mmap counters remain logical mapped bytes and
are not relabeled as page-cache residency.

Alternatives rejected: `/proc/meminfo` is host-wide; `/proc/self/status` omits
ordinary pread page cache; per-file `mincore` scans are expensive and cannot
cover files read by other instances reliably.

### D6: Bound non-reclaimable memory outside the cache policy

Documentation treats the response-buffer bound as
`max_inflight × cap_bytes`, recommends a nonzero global `max_inflight`, and then
sets cgroup `MemoryHigh`/`MemoryMax` with room for response buffers, sockets,
runtime baseline, and a reclaimable cache window. The server observes but does
not write cgroup controls.

For the current 8 MiB cap, the report uses `max_inflight = 16` (128 MiB response
budget), `MemoryHigh = 512 MiB`, and `MemoryMax = 768 MiB` as an example, not as
universal defaults.

## Risks / Trade-offs

- [Repeated downloads lose cache hits under `drop_after_read`] → Keep `normal`
  as default, make the policy explicit, benchmark repeated-reader throughput,
  and document workload selection.
- [Concurrent readers race with advisory eviction] → Treat advice as best
  effort, advise only this request's fully read pages, preserve response buffers,
  and test overlapping readers for integrity.
- [Advice succeeds but pages remain resident] → Report attempts separately from
  cgroup `mem_file`; use cgroup limits for the hard total-memory boundary.
- [Tight cgroup limits cause OOM because anonymous buffers are not reclaimable]
  → Require `max_inflight` capacity planning and deploy `MemoryHigh` before a
  safely higher `MemoryMax`.
- [cgroup files are unavailable or the service shares a cgroup] → Emit `-1` for
  unavailable data and document that `mem_file` is cgroup-wide, not per-file.
- [Additional stats parsing or formatting regresses the hot path] → Sample only
  during stats emission and benchmark the unchanged `normal` request path.

## Migration Plan

1. Ship with `page_cache_policy = normal`; verify new counters and cgroup gauges
   in staging without changing cache behavior.
2. Configure and validate nonzero `max_inflight`, then deploy cgroup
   `MemoryHigh`/`MemoryMax` using the report's budget formula.
3. Enable `noreuse` for a canary serving one-shot artifacts and compare cache
   residency, disk reads, throughput, and advice errors.
4. Enable `drop_after_read` only if the canary meets integrity and performance
   gates; expand gradually while watching disk latency and repeated downloads.
5. Roll back instantly by setting `page_cache_policy = normal`; no data or wire
   migration is required.

## Open Questions

- Production filesystems and kernels may respond differently to `NOREUSE` and
  `DONTNEED`; final rollout thresholds require a production-like canary.
- If multiple ferry instances share one cgroup, deployment must decide whether
  aggregate `mem_file` is sufficient or each instance needs a separate unit.
- A future change may coordinate eviction by inode/offset across readers if
  overlapping-download measurements show material cache thrashing.
