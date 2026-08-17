# Tasks: add-ferry-client

Parallelization note (per requirement): group 1 is the serial prerequisite; **all of group 2 is mutually parallel** (independent files, tests alongside, no cross-deps); group 3 is parallel to group 2 and within itself; groups 4→5→6→7 are the serial integration tail. Module boundaries follow design D9.

## 1. Scaffold & build (serial prerequisite)

- [x] 1.1 Root `xmake.lua` adds `includes("client")`; write `client/xmake.lua` with `ferry-client-core` (static: all `client/*.cc` except main) and `ferry-client` (binary) targets; placeholder module sources so the build is green
- [x] 1.2 Define `ClientConfig` (all knobs with defaults: jobs 4, chunk 8 MiB, receive-timeout 60 s, single-stream-limit 256 MiB) and a minimal `main.cc` that parses args and prints the config; verify `xmake run ferry-client --help`-style invocation (NOTE: `ClientConfig` ownership delegated to task 2.4's `cli.{h,cc}` to keep the parallel lane conflict-free; scaffold `main.cc` is a stub replaced in §5)

## 2. Pure modules — PARALLEL LANES (each: one .h/.cc pair + its unit-test file, no cross-dependencies)

- [x] 2.1 `planner.{h,cc}`: chunk count/offset/length math for a given size+chunk_size, effective workers = min(jobs, chunks); unit tests (size 0/edge: size < chunk, exact multiple, +1 boundary)
- [x] 2.2 `bitmap.{h,cc}`: bitmap set/test/count; meta JSON build/parse (url, size, last_modified, chunk_size, bitmap, version); atomic persist (temp + rename); unit tests (round-trip, corrupt/missing meta, version mismatch rejection)
- [x] 2.3 `backoff.{h,cc}`: failure classification (transient/429/fatal/success/mismatch) and delay sequence `min(500ms·2^n, 30s)` with 8-attempt cap, `max(Retry-After, backoff)` rule; unit tests (sequence values, cap, attempt exhaustion, Retry-After floor)
- [x] 2.4 `cli.{h,cc}`: full argument parsing/validation per spec (`-o -j --chunk-size --checksum --no-verify --receive-timeout --single-stream-limit -q`, URL basename default for output); unit tests (defaults, invalid values, checksum format check)
- [x] 2.5 `probe.{h,cc}` (pure decision layer): function over (HEAD outcome, probe-GET outcome) → mode enum {CHUNK, SINGLE_STREAM, REFUSE_OVERSIZE, RETRYABLE}; unit tests covering the full decision matrix incl. missing CL, 405 HEAD, no Accept-Ranges
- [x] 2.6 `progress.{h,cc}`: stats struct (done bytes/chunks/retries/total), one-line formatter matching the spec format, final summary formatter; unit tests (format strings, percent/ETA math, unknown-size case)

## 3. Workflow-dependent leaf modules (parallel to group 2 and each other)

- [x] 3.1 `verify.{h,cc}`: streaming sha256 over a file via chained pread tasks feeding OpenSSL EVP (bounded buffer); synchronous wrapper via WaitGroup; unit tests against known sha256 vectors (empty, small pattern, multi-chunk file)
- [x] 3.2 Probe request layer (in `probe.{h,cc}` or `engine`): HEAD request + `Range: bytes=0-0` probe via `create_http_task` extracting status/Content-Length/Accept-Ranges/Last-Modified/chunked flag; covered by L2 (6.x)

## 4. Engine — THE SEAM (serial; consumes all of groups 2–3)

- [x] 4.1 `engine.{h,cc}` worker loop: repeater claiming → http GET Range → validate 206 (start/length/total per spec) → pwrite at chunk offset → bitmap mark → next; keep-alive per worker; per-task receive timeout from config
- [x] 4.2 Retry integration: classify outcomes via backoff module, timer-task delays in-series, per-chunk attempt counter, Retry-After handling
- [x] 4.3 Fatal paths + stop flag: 403/404, total-mismatch, attempts-exhausted → broadcast stop, drain workers, surface exit reason
- [x] 4.4 Single-stream mode: probe-body handoff (200 full body is the download start), stream-to-disk with size-limit enforcement, chunked decode via `HttpUtil::decode_chunked_body`
- [x] 4.5 Resume integration: load+validate meta (HEAD size/Last-Modified compare; mismatch → discard+warn), persist on each chunk mark, SIGINT → stop + final flush + resume hint
- [x] 4.6 Completion: sha256 gate (--checksum compare; mismatch keeps files), rename `.part` → output, remove meta, final summary; progress ticker start/stop

## 5. Main wiring

- [x] 5.1 `main.cc`: config → probe → plan/resume decision → engine run → verify → exit codes; startup line (url, size, mode, jobs)
- [x] 5.2 Signal handling end to end: SIGINT mid-run leaves resumable state, exits non-zero with the resume hint

## 6. L2 in-process integration tests (real server handler + client engine, same process)

- [x] 6.1 Harness: start `ferry::Handler` on an ephemeral port (reuse the server-suite pattern) + helper driving the client engine against a URL with configurable client options
- [x] 6.2 Happy paths: full download sha256-matches source; server cap ≪ file (capped cycling); jobs 1/4/16; file sizes: <chunk, ==chunk, exact multiple, +1
- [x] 6.3 Rate-limit paths: tiny server rate → download succeeds slowly within tolerance; engineered 429 (huge request over tiny max_wait… via engine knob) → Retry-After waited then success
- [x] 6.4 Resume paths: destroy engine mid-download, restart → completes, hash matches; server mtime bumped between runs → state discarded + fresh start
- [x] 6.5 Fatal paths: 403 (blacklist the client IP in the server ACL) and 404 → non-zero exit, `.part` kept, workers stop

## 7. L3 system tests (real binaries; extend tests/system/)

- [x] 7.1 `run_client_l3.sh`: ferry-server + ferry-client end-to-end on the 64 MiB pattern file: completes, sha256 matches, `--checksum` pass and deliberate-mismatch (non-zero exit, files kept, no output)
- [x] 7.2 SIGKILL the client mid-download, re-run same command → resumes and completes; hash matches; elapsed chunks ≥ previously completed (no full restart)
- [x] 7.3 Interop: python `http.server` (foreign Range server) download + hash; Range-less stub server → single-stream success under limit and refusal over limit
- [x] 7.4 UX/misuse: progress lines appear (and `-q` silences them); invalid CLI (`-j 0`, bad checksum format) exits non-zero with clear message

## 8. Documentation

- [x] 8.1 README client section: usage + CLI reference, resume semantics (.part/.ferry.json), timeout-coupling note (client receive-timeout > server max_wait_sec), per-IP-rate-limit expectation (concurrency hides latency, doesn't multiply bandwidth), single-stream limit rationale
