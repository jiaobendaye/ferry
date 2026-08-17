# Proposal: add-ferry-client

## Why

ferry-server serves files as capped Range responses (every 206 carries the full size in `Content-Range`; requests longer than the cap get legally-shorter ranges back). A paired download client is needed to exploit that contract: multi-threaded, resumable, and robust to the server's bandwidth shaping. The client is also designed to work against any RFC-conformant Range server, making it a general-purpose tool rather than a private companion.

## What Changes

- New CLI binary `ferry-client <url>` under `client/` (xmake target), built on Workflow's HTTP client tasks, sharing no code with `server/` (the two are mirrors of each other's Range logic; extracting a `common/` library was considered and rejected).
- **Model B chunking**: fixed `chunk_size` (default 8 MiB, CLI-configurable) is the single unit of memory, concurrency, resume, and retry. Chunks are claimed dynamically from an atomic counter by `jobs` workers (default 4), each a `WFRepeaterTask` loop; completion tracked in a bitmap.
- **Generic Range-server compatibility**: probe matrix (HEAD → Range probe → 206) decides multi-chunk mode; servers without Range support fall back to single-stream mode with a configurable size limit (`--single-stream-limit`, default 256 MiB) to keep Workflow's whole-response buffering from exhausting memory. The probe doubles as the download start when a non-Range server answers a probe with the full body.
- **Resume**: `<output>.part` data file + `<output>.ferry.json` meta (url, size, Last-Modified, chunk_size, bitmap). On restart, HEAD verifies size + Last-Modified match before reusing state; mismatch restarts from scratch with a warning. SIGINT flushes the bitmap and exits cleanly; kill -9 is also survivable (bitmap reflects last durably marked chunk).
- **Chunk-level retries with exponential backoff**: 500 ms × 2^n capped at 30 s, max 8 attempts per chunk; `429` responses wait `max(Retry-After, backoff)`; `403`/`404` are fatal and broadcast stop to all workers. Receive timeout defaults to 60 s so the server's soft-shaping delays (up to its `max_wait_sec`) are never mistaken for failures — a documented cross-component coupling.
- **Final verification**: streaming sha256 over the assembled file (async pread chain, bounded memory), always computed and printed; compared against `--checksum sha-256=<hex>` when given; only a passing result renames `.part` to the output file. `--no-verify` skips the pass.
- **Numeric progress**: one line per second on stderr (`percent, done/total, speed, ETA, chunks done/total, retries`).
- **Testing emphasis** (per user requirement): L1 unit tests for every pure module (planner, Content-Range parser, bitmap/metafile, backoff, probe decision matrix), L2 in-process closed-loop tests running the real ferry-server handler against the client engine (including rate-limit/429 paths via controlled config), L3 real-binary tests: client↔server downloads, SIGKILL-mid-download resume with hash verification, and interop with a foreign Range server (python http.server) plus a no-Range fallback server.
- **Parallelizable implementation** (per user requirement): tasks are structured so independent modules live in independent files with unit tests alongside, allowing several implementers/agents to work concurrently; integration happens at a defined seam (the downloader engine).

## Capabilities

### New Capabilities

- `resumable-download-client`: multi-threaded chunked downloading with dynamic work claiming, resume persistence, generic Range-server probing and single-stream fallback, chunk-level retry/backoff, final sha256 verification, and progress reporting.

### Modified Capabilities

(none — ferry-server's requirements are unchanged)

## Impact

- **New code**: `client/` sources (cli/main, probe, planner, chunk engine, bitmap/metafile persistence, backoff, verifier, progress), `client/xmake.lua`, test additions under `tests/unit`, `tests/integration`, `tests/system`.
- **Build**: root `xmake.lua` gains `includes("client")`; no new external dependencies (Workflow + OpenSSL already required; sha256 via OpenSSL EVP).
- **Docs**: README gains client usage, CLI reference, and the timeout-coupling note (`client receive timeout > server max_wait_sec`).
- **Operational coupling**: against a rate-limited ferry-server, all of one client's connections share the server's per-IP token bucket — concurrency hides latency but does not multiply bandwidth; documented to set expectations.
