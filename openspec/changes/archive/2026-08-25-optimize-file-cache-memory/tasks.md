## 1. Configuration and Policy Model

- [x] 1.1 Add the `FileCachePolicy` enum, name conversion, `normal` default, and `page_cache_policy` parsing with allowed-value errors.
- [x] 1.2 Reject non-normal cache policies combined with mmap mode and include the effective policy in the startup log.
- [x] 1.3 Add configuration unit tests for all policy values, the default, invalid input, and mmap compatibility.

## 2. Pread Cache Advice

- [x] 2.1 Add an injectable cache-advisor abstraction that wraps `posix_fadvise` and computes overflow-safe, fully covered page-aligned intervals.
- [x] 2.2 Integrate `POSIX_FADV_NOREUSE` before scheduled pread and `POSIX_FADV_DONTNEED` after successful pread but before fd close, leaving normal and error paths unchanged.
- [x] 2.3 Add cumulative advice call, accepted-byte, and error counters without changing successful HTTP responses on advice failure.
- [x] 2.4 Add unit tests for aligned/unaligned/empty/short/overflow-edge ranges, policy timing, read failure, advice failure, and counter semantics.
- [x] 2.5 Add integration tests proving identical status, headers, lengths, and body hashes for normal, noreuse, and drop-after-read under non-overlapping and overlapping Range requests.

## 3. Cgroup Memory Observability

- [x] 3.1 Implement an injectable cgroup-v2 resolver/parser for `/proc/self/cgroup`, `/proc/self/mountinfo`, and `memory.stat` fields `anon`, `file`, and `sock`.
- [x] 3.2 Sample cgroup memory only when periodic or SIGUSR1 stats are emitted and append `mem_anon`, `mem_file`, `mem_sock`, and cache-advice counters to the existing line.
- [x] 3.3 Emit `-1` independently for unavailable fields and keep serving/logging when discovery, reads, or parsing fail.
- [x] 3.4 Add unit tests for normal and escaped cgroup paths, nonstandard cgroup2 mountpoints, missing controllers/files/fields, malformed values, and stats-line backward compatibility.

## 4. Documentation and Deployment Guidance

- [x] 4.1 Document all cache policies, best-effort semantics, mmap incompatibility, startup output, stats fields, and workload-selection trade-offs in both READMEs and the sample config.
- [x] 4.2 Document the `max_inflight × cap_bytes` anonymous-memory formula, cgroup `MemoryHigh`/`MemoryMax` examples, monitoring separation, canary rollout, rollback, and the prohibition on production `drop_caches`.
- [x] 4.3 Update `optimization-report.md` with post-implementation measurements, exact test commands/environment, observed cache reduction, throughput, CPU/GiB, disk reads, and any threshold deviations.

## 5. Verification and Performance Gates

- [x] 5.1 Add an opt-in cold-file stress test that records process RSS/RssAnon, target-file residency, cgroup memory classes, advice counters, integrity, and the idle cache threshold for each policy.
- [x] 5.2 Add an opt-in repeated-reader benchmark comparing first and second reads so drop-after-read disk-I/O and throughput costs are explicit.
- [x] 5.3 Run unit, integration, system, ASan nocopy-lifetime, and existing pread/mmap stress coverage with the default normal policy.
- [x] 5.4 Demonstrate that normal-mode throughput regresses by no more than 3%, CPU/GiB by no more than 5%, drop-after-read cold throughput by no more than 10%, and the 500 MiB idle residency meets `max(64 MiB, 2 × cap_bytes)`.
