# Design: add-ferry-client

## Context

ferry-server is implemented and its requirements are archived as main specs (`openspec/specs/`: range-file-serving, ip-access-control, bandwidth-rate-limiting, client-ip-resolution). The server contract the client consumes:

- HEAD returns full `Content-Length` for any existing file regardless of size threshold.
- Every 206 carries `Content-Range: bytes s-e/TOTAL`; ranges longer than the server's cap come back as legally-shorter ranges (RFC 9110), so a client never needs to know the cap.
- Rate limiting is per-IP soft shaping: responses slow down rather than fail, until the deficit exceeds `max_wait_sec` (default 30 s) → `429` + `Retry-After`.
- `416` with `bytes */SIZE` when an offset is past EOF (resume-completion signal).

Constraints from the framework (verified during the server work): Workflow HTTP **client tasks buffer the whole response message in memory**; `WFRepeaterTask` supports create-until-NULL loops; file IO is async via `create_pread_task`/`create_pwrite_task`; redirects and retries are built into `create_http_task(url, redirect_max, retry_max, cb)`.

User requirements shaping this design: full unit + integration test coverage, and an implementation structure that **can be parallelized across agents** (independent modules in independent files, tests alongside, one defined integration seam).

## Goals / Non-Goals

**Goals:**
- Multi-threaded, resumable downloads against ferry-server and any RFC-conformant Range server.
- Memory bounded by configuration in every mode: `jobs × chunk_size` in Range mode; `single_stream_limit` in fallback mode.
- Survivable interruption: SIGINT and kill -9 both leave a resumable `.part` + meta state.
- Correctness-gated completion: output file appears only after streaming sha256 passes (against the user's checksum when provided).
- Chunk-level resilience: exponential backoff, Retry-After honored, fatal errors stop the fleet fast.

**Non-Goals:**
- Multi-URL / recursive downloads (the server has no directory listing anyway).
- HTTPS client configuration niceties beyond Workflow defaults.
- Metalink/magnet, per-chunk hashing (sha256 of the whole file only).
- Extracting shared code with `server/` — the two are mirrors (server parses Range / emits Content-Range; client emits Range / parses Content-Range) and share nothing worth a `common/` library.

## Decisions

### D1: Model B — fixed chunk size, dynamic claiming, bitmap

`chunk_size` (default 8 MiB) is the single unit of memory, concurrency, resume, and retry. The file maps to `ceil(size / chunk_size)` fixed chunks. Workers claim the next unclaimed chunk via an atomic counter; completion lives in a bitmap.

**Why over static region partitioning (Model A):** with B, a slow chunk doesn't strand a dedicated worker tail-first; balance is automatic. State complexity is comparable — a bitmap is simpler to persist durably than N per-region progress offsets. Note that against a rate-limited ferry-server all connections share one per-IP bucket, so concurrency hides latency rather than multiplying bandwidth — load balance still matters for network jitter and for unlimited servers.

Memory bound follows directly: a 206 response is never longer than the requested range (client verifies `Content-Range` length vs body length), so peak ≈ `jobs × chunk_size`.

### D2: Worker mechanics on Workflow

Each worker is a `WFRepeaterTask` (tutorial-09 pattern): its `create` function claims the next incomplete chunk (returns NULL when none remain or a stop flag is set) and builds the chunk's series: `http_task(Range) → validate → pwrite_task → mark bitmap`. Looping by re-extending the series inside callbacks; per-chunk retry state rides in a small heap context freed in the terminal callback.

Stop broadcast: a shared atomic flag set on fatal errors or SIGINT; `create` checks it before claiming. Completion coordination via `WFFacilities::WaitGroup` in main.

Connections: keep-alive on each worker's tasks (each worker's sequential chunks reuse one connection through the proxy). `redirect_max = 5`, framework retry disabled at task level (`retry_max = 0`) because retry policy belongs to D5 with backoff and status-specific behavior.

### D3: Probe matrix and single-stream fallback (generic compatibility)

```
HEAD url
 ├─ 200 + Content-Length + Accept-Ranges: bytes → chunk mode (skip probe GET)
 ├─ 200 + Content-Length, no Accept-Ranges     → probe GET Range: bytes=0-0
 ├─ 405 / no Content-Length / failure          → probe GET Range: bytes=0-0
probe GET Range: bytes=0-0
 ├─ 206 → chunk mode (discard the 1-byte probe body)
 └─ 200 → server ignores Range: the response IS the full download
          → single-stream mode, stream it to disk as the beginning
```

**Single-stream memory cliff:** Workflow buffers the entire response, so single-stream mode is capped by `--single-stream-limit` (default 256 MiB): if the probe HEAD gave a Content-Length above the limit, refuse before downloading; if size is unknown, abort mid-stream the moment the limit is exceeded. Rationale: this product exists for Range; the fallback is best-effort for small files.

Chunked transfer-encoded responses (generic servers) are decoded with `HttpUtil::decode_chunked_body` before writing.

### D4: Resume persistence

Files: `<output>.part` (data, sparse-friendly pwrite at chunk offsets) + `<output>.ferry.json` (meta: url, size, last-modified, chunk_size, bitmap, version).

- **Marking order:** a chunk is marked complete in the bitmap only after its pwrite succeeds; the meta file is rewritten after each marking (atomic replace: write temp + rename). Worst case after kill -9: one finished-but-unmarked chunk re-downloads. No corruption either way — chunks are disjoint fixed intervals.
- **Restart:** HEAD → compare size and Last-Modified with meta. Match → resume from bitmap. Mismatch → warn, delete `.part`/meta, restart. Missing/corrupt meta → same.
- **SIGINT:** sets the stop flag; workers finish their in-flight chunk or abandon it (unmarked), main flushes final meta state, exits non-zero with a "resume with the same command" note.
- URL redirects: meta stores the URL the user gave; redirects are re-followed on resume (v1 simplicity).

### D5: Retry taxonomy and exponential backoff

Per-chunk, attempts counted from 0:

| Outcome | Action |
|---|---|
| Network error / timeout / 5xx | backoff `min(500ms × 2^attempts, 30s)`, retry; give up after 8 attempts → overall failure (`.part` kept) |
| `429` | wait `max(Retry-After seconds, backoff value)`, retry (same attempt counter) |
| `206` with mismatched start/length/total | re-validate once; persistent mismatch → treat as corruption of expectations → overall failure |
| `416` at offset == size | chunk (and file) already complete — normal resume race |
| `403` / `404` | fatal: set stop flag, all workers exit, non-zero exit, `.part` kept |

Backoff waits are `WFTimerTask`s in the chunk's series — no thread blocks.

**Timeout coupling:** per-task receive timeout defaults to **60 s** and must stay larger than the server's `max_wait_sec` (30 s default) plus transfer time, or soft-shaped responses read as client-side timeouts and trigger useless retries. Documented in README; CLI override `--receive-timeout`.

### D6: Final verification and rename gating

After all chunks complete: streaming sha256 over the `.part` file via a chain of pread tasks feeding OpenSSL EVP (bounded memory, async). The digest is always computed and printed unless `--no-verify`. With `--checksum sha-256=<hex>` the digest must match; on mismatch the `.part` and meta are **kept** and the exit code is non-zero (preserve evidence; differs from aria2's delete). Only a passing (or skipped) verification renames `.part` → output and removes the meta file.

### D7: CLI and progress

```
ferry-client [options] <url>
  -o, --output PATH           output file (default: URL basename)
  -j, --jobs N                workers (default 4; effective = min(jobs, chunks))
  --chunk-size BYTES          default 8MiB
  --checksum sha-256=HEX      expected digest
  --no-verify                 skip final sha256 pass
  --receive-timeout SEC       default 60
  --single-stream-limit BYTES default 256MiB
  -q, --quiet                 suppress progress lines
```

Progress: one stderr line per second —
`45.2%  378.0MiB/834.0MiB  52.3MiB/s  ETA 9s  chunks 48/105  retries 3`;
final summary line: elapsed, average speed, sha256.

### D8: Testing architecture

- **L1 unit** (no network): chunk planner (counts, min(jobs,chunks), edge sizes 0/<chunk/exact multiples), Content-Range parser (valid/malformed/mismatched totals), bitmap + metafile serialize/round-trip/atomic-replace semantics, backoff sequence math, probe decision matrix (pure function over HEAD/probe outcomes), CLI parse.
- **L2 in-process closed loop** (the distinctive opportunity): run the real `ferry::Handler` as an in-process server (ephemeral port, the server suite's harness) and drive the client engine against it: full downloads with sha256 match; capped-range cycling (server cap ≪ file); rate-limited run (small rate → slow but successful); engineered 429 → Retry-After path; resume by destroying the downloader mid-run and restarting against the same server; 403/404 fatal propagation.
- **L3 real binaries**: ferry-client ↔ ferry-server end-to-end (64 MiB pattern file, hash-verified); **SIGKILL-mid-download then resume** to completion + hash; interop against python `http.server` (Range-capable foreign server); single-stream fallback against a deliberately Range-less stub server; progress/quiet flags; `--checksum` mismatch → non-zero exit, file kept.

### D9: Module decomposition for parallel implementation

Independent files, each with its own unit test file; only the engine touches Workflow tasks:

```
client/
├── cli.{h,cc}        # argument parsing → ClientConfig (pure)
├── probe.{h,cc}      # probe decision matrix + HEAD/probe request logic
├── planner.{h,cc}    # chunk math: count/offsets/claims (pure)
├── bitmap.{h,cc}     # bitmap + metafile persistence (atomic replace)
├── backoff.{h,cc}    # retry classification + delay sequence (pure)
├── verify.{h,cc}     # streaming sha256 over pread chain
├── progress.{h,cc}   # stats aggregation + line formatting (pure-ish)
├── engine.{h,cc}     # THE SEAM: repeater workers, series assembly, stop flag
└── main.cc           # wiring only
```

`engine.{h,cc}` depends on everything; everything else depends on nothing but std/OpenSSL. This lets agents implement cli/planner/bitmap/backoff/probe-decision/verify/progress **concurrently** after the scaffold task, with engine + main + L2 tests as the serial integration tail.

## Risks / Trade-offs

- [Workflow buffers whole responses] → Range mode bounded by chunk_size (client verifies); single-stream mode bounded by `single_stream_limit`; both documented.
- [Concurrency does not multiply bandwidth against a rate-limited ferry-server (per-IP bucket)] → documented expectation-setting; concurrency still hides latency and helps unlimited servers.
- [kill -9 between pwrite and meta rewrite] → at most one chunk re-downloaded; disjoint chunks make this safe by construction.
- [File changes on the server mid-download (mtime/size drift)] → HEAD check on restart; `Content-Range` total mismatch during download is fatal rather than silently wrong.
- [Redirects make the stored URL stale across restarts] → v1 re-follows redirects from the original URL; acceptable.
- [sha256 pass doubles read IO for huge files] → streaming and async; `--no-verify` opt-out.
- [Generic-server edge cases (HTTP/1.0, no Content-Length, chunked 200)] → covered by the probe matrix and single-stream path; unknown-size downloads report bytes without percent and obey the size limit.

## Open Questions

(none blocking — defaults all fixed in explore; tuning constants live in `cli` defaults.)
