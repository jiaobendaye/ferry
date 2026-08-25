# file-cache-management Specification

## Purpose
TBD - created by archiving change optimize-file-cache-memory. Update Purpose after archive.
## Requirements
### Requirement: Configurable file-cache policy
The server SHALL accept `page_cache_policy = normal`, `noreuse`, or
`drop_after_read`, defaulting to `normal` when unset. Unknown values SHALL cause
startup to fail with an error naming the key and allowed values, and the
effective policy SHALL appear in the startup log.

#### Scenario: Default preserves existing behavior
- **WHEN** `page_cache_policy` is omitted
- **THEN** the effective policy is `normal` and the server starts successfully

#### Scenario: Invalid policy fails startup
- **WHEN** `page_cache_policy` contains any value other than `normal`, `noreuse`, or `drop_after_read`
- **THEN** startup fails with an error that names `page_cache_policy` and the allowed values

### Requirement: Normal policy performs no cache advice
Under the `normal` policy the server SHALL issue neither `POSIX_FADV_NOREUSE`
nor `POSIX_FADV_DONTNEED` for file responses, preserving the existing buffered
I/O cache behavior.

#### Scenario: Normal range response
- **WHEN** a Range response completes successfully under `normal`
- **THEN** its body is served normally and no file-cache advice call is made

### Requirement: Noreuse policy advises the decided read range
Under the `noreuse` policy the pread body path SHALL issue
`POSIX_FADV_NOREUSE` for the decided response range before reading it. The hint
SHALL NOT alter the requested offset, response length, status, headers, or body.

#### Scenario: Noreuse range response
- **WHEN** a satisfiable Range request selects offset S and length N under `noreuse`
- **THEN** the server advises `[S, S+N)` as non-reusable and returns the same response bytes and metadata as `normal`

### Requirement: Drop-after-read advises successfully copied full pages
The server SHALL, under the `drop_after_read` policy and after a successful
pread but before closing the file descriptor, issue `POSIX_FADV_DONTNEED` for the
largest page-aligned interval fully contained in the bytes actually read. It
SHALL NOT advise unread bytes or expand the interval into adjacent pages, and
SHALL keep the copied anonymous response buffer alive until HTTP completion.

#### Scenario: Aligned successful read is dropped
- **WHEN** a successful pread returns an aligned interval of N bytes under `drop_after_read`
- **THEN** the complete returned interval is advised with `POSIX_FADV_DONTNEED` before fd close and the response remains byte-for-byte correct

#### Scenario: Unaligned successful read preserves boundary pages
- **WHEN** a successful pread begins or ends within a page under `drop_after_read`
- **THEN** only fully covered interior pages are advised and the partial boundary pages are not expanded into

#### Scenario: Failed read is not dropped
- **WHEN** pread fails or returns no successfully copied full page
- **THEN** no `POSIX_FADV_DONTNEED` call is made for that result and the existing response/error behavior is preserved

### Requirement: Cache advice is best effort and observable
Failure of a supported cache-advice syscall SHALL NOT fail or modify an
otherwise successful HTTP response. The server SHALL maintain cumulative advice
call, successfully advised byte, and advice error counters; advised bytes SHALL
mean bytes accepted by the syscall, not bytes proven physically evicted.

#### Scenario: Advice succeeds
- **WHEN** a cache-advice syscall succeeds for N bytes
- **THEN** the advice call counter increments once, advised bytes increments by N, and the advice error counter does not increment

#### Scenario: Advice fails
- **WHEN** an injected or operating-system cache-advice call returns an error
- **THEN** the response remains correct, the call and error counters increment once, and advised bytes does not increment

### Requirement: Non-normal policy requires pread mode
The server SHALL reject a configuration that combines `file_body_mode = mmap`
with `page_cache_policy = noreuse` or `drop_after_read`, because the mmap path
does not provide equivalent post-read advice semantics.

#### Scenario: Drop policy with mmap is rejected
- **WHEN** startup configuration selects `file_body_mode = mmap` and `page_cache_policy = drop_after_read`
- **THEN** startup fails with an error naming both incompatible settings

#### Scenario: Normal policy with mmap remains supported
- **WHEN** startup configuration selects `file_body_mode = mmap` and `page_cache_policy = normal`
- **THEN** the existing experimental mmap path remains available

### Requirement: Bounded-memory deployment guidance
Operator documentation SHALL distinguish non-reclaimable response buffers from
reclaimable cgroup file memory and SHALL document capacity planning with
`max_inflight × cap_bytes`, cgroup `MemoryHigh`/`MemoryMax`, policy workload
selection, and rollback to `normal`. It SHALL prohibit production use of global
`drop_caches` as a per-service control.

#### Scenario: Operator plans a memory budget
- **WHEN** an operator consults the deployment documentation
- **THEN** they can calculate the anonymous response-buffer bound, leave headroom for sockets and baseline memory, select a cache policy by workload, and identify the rollback procedure
