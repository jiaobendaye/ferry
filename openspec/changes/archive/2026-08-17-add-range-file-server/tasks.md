# Tasks: add-range-file-server

## 1. Project scaffold & build

- [x] 1.1 Create repo layout: `server/` sources dir, `config/` for server config + ACL file samples, `tests/unit/` + `tests/integration/` dirs, reserve empty `client/` placeholder
- [x] 1.2 Write root `xmake.lua` and `server/xmake.lua`; define two targets: `ferry-server-core` (static library: config/acl/range/rate_limiter/client_ip/handler) and `ferry-server` (binary: + `main.cc`, links core, pthread/dl). (NOTE: initially wired a local `~/lab/workflow` prefix via a `workflow_prefix` option; later switched to the xrepo `workflow v1.0.1` package — see design D6.)
- [x] 1.3 Set up gtest via xrepo (`add_requires("gtest", {configs = {main = true}})`, mirroring workflow's own `test/xmake.lua`); define `unit-test` and `integration-test` binary targets under `tests/` linking `ferry-server-core`, building green with placeholder cases
- [x] 1.4 Minimal `main.cc` that starts a `WFHttpServer` answering a hello 200; verify `xmake && xmake run ferry-server` works end to end
- [x] 1.5 Verify graceful shutdown on SIGINT/SIGTERM via `WFFacilities::WaitGroup` (tutorial-09 pattern)

## 2. Configuration module

- [x] 2.1 Implement `config.{h,cc}`: flat `key = value` parser, trim/comments, lookup API
- [x] 2.2 Define all keys with defaults: `port`, `root`, `cap_bytes` (8 MiB), `size_threshold_bytes` (=cap), `rate_bytes_per_sec`, `max_wait_sec` (30), `trust_hops` (1), `acl_file`, `acl_poll_interval_sec` (5), `max_connections`
- [x] 2.3 Fail startup loudly on missing file, unknown-key warning policy, and invalid values (negative/non-numeric); unit-test each bad-value path

## 3. Client IP resolution (`client_ip.{h,cc}`)

- [x] 3.1 Parse `X-Forwarded-For`, select `trust_hops`-th entry from the right; parse IPv4/IPv6 literals into a normalized form (`in6_addr`-based, v4-mapped)
- [x] 3.2 Fallback to `server_task->get_peer_addr()` when header absent, entry unparseable, or header shorter than `trust_hops`
- [x] 3.3 Unit tests: forged-leftmost ignored, rightmost selected, trust_hops=2, garbage entry, missing header, IPv6 normalization equivalence

## 4. ACL module (`acl.{h,cc}`)

- [x] 4.1 Parse CIDR entries (IPv4/IPv6, bare IP → /32 or /128) into network/prefix pairs; reject unparseable lines
- [x] 4.2 Match function with blacklist-priority semantics: blacklist hit → deny; non-empty whitelist without hit → deny; else allow
- [x] 4.3 Active rule set held behind atomic swap (shared_ptr or lock) so readers always see a complete set
- [x] 4.4 mtime-poll reload task (timer, `acl_poll_interval_sec`): parse new file fully before swap; parse failure keeps old rules and logs
- [x] 4.5 Startup behavior: missing/broken configured ACL file → refuse to start; empty file → allow all
- [x] 4.6 Unit tests: v4 subnet, v6 prefix, bare IP, blacklist-over-whitelist, empty whitelist allows, parse-failure rejection

## 5. Range parsing module (`range.{h,cc}`)

- [x] 5.1 Parse `Range` header: unit check (`bytes` only), first-byte/last-byte positions, open-ended (`N-`), suffix (`-N`); multiple ranges → keep first only; syntactically invalid → "no range"
- [x] 5.2 Apply cap and file size: compute final `(status, offset, length)` — 206 with length ≤ cap; 416 when start ≥ size; decide 200 vs 413 for non-Range via threshold
- [x] 5.3 Unit tests against every spec scenario: exact range, capped open-ended, capped wide range, suffix within/beyond file, past-end → 416, multi-range → first, invalid unit ignored, threshold boundary (size == threshold → 200)

## 6. Rate limiter (`rate_limiter.{h,cc}`)

- [x] 6.1 Per-IP token bucket (rate from `rate_bytes_per_sec`) with an **injectable time source** (production = steady_clock, tests = fake clock); mutex-guarded map; lazy refill on access
- [x] 6.2 `reserve(bytes)` → required wait `t = deficit/rate`; return delay or "over max_wait" (→ 429); zero/unset rate disables limiting entirely
- [x] 6.3 Idle-entry sweep via periodic timer task (entries idle beyond threshold are erased)
- [x] 6.4 Unit tests with fake clock (no sleeping): immediate within budget, correct wait on deficit, over-budget → reject signal, idle reclaim, disabled mode

## 7. Request handler (`handler.{h,cc}`)

- [x] 7.1 Pipeline skeleton: resolve IP → ACL → path safety → stat → range decision → limiter → series assembly; wire 403/400/404/413/416/429 responses
- [x] 7.2 Path safety: percent-decode URI path, reject NUL and any resolution escaping `root` (400); unit-test decode + containment separately
- [x] 7.3 Response metadata on all responses: explicit `Content-Length`, `Accept-Ranges: bytes`, `Last-Modified` when stat succeeded; `Content-Range` for 206 and 416 (`bytes */N`)
- [x] 7.4 200/206 body path: open fd in process, `create_pread_task(fd, buf, len, offset, cb)`, `append_output_body_nocopy` in callback, close fd in pread callback, free buf via `server_task->set_callback` — verify every path (including pread failure → 503)
- [x] 7.5 Soft shaping: when limiter returns delay ≤ max_wait, push `WFTimerTask(delay)` into the series before pread; over-budget → 429 + `Retry-After`
- [x] 7.6 HEAD support: same headers as GET (200 + full `Content-Length` regardless of threshold), no body, no pread
- [x] 7.7 `If-Range` with `Last-Modified`: stale validator → treat as non-Range request
- [x] 7.8 413 body text: self-describing, includes file size and instructs to use Range requests
- [x] 7.9 Apply `max_connections` via `WFServerParams` at server start

## 8. Main & lifecycle

- [x] 8.1 Load config → build ACL/limiter singletons → start reload/sweep timers → start server with configured port and params
- [x] 8.2 Startup logging: effective config summary (port, root, cap, threshold, rate, trust_hops, ACL entry counts)
- [x] 8.3 Signal handling: SIGINT/SIGTERM → stop timers, `server.stop()`, clean exit

## 9. In-process integration tests (L2)

- [x] 9.1 Test harness: start `WFHttpServer` on an ephemeral port (`start(0)` + `get_listen_addr()`), helper to fire workflow HTTP client tasks with arbitrary headers and return status/headers/body; per-test config with shrunk knobs (tiny rate, `max_wait_sec=1`, poll interval 1 s)
- [x] 9.2 Status-machine coverage: 200 whole, 206 exact/capped (assert `Content-Range` + body bytes), suffix, past-end 416 (`bytes */N`), multi-range → first, invalid unit → non-Range rule, 413 large-file no-Range, 404, traversal 400
- [x] 9.3 HEAD: large file → 200 + full `Content-Length` + empty body; small file; missing → 404
- [x] 9.4 XFF injection: blacklist rightmost IP while forging a whitelisted leftmost entry → 403; no XFF → peer-address behavior
- [x] 9.5 Soft shaping: tiny rate, request needing short wait → assert 206 arrives within generous tolerance window [expected×0.5, expected×4]; engineered deficit > max_wait → 429 + `Retry-After`
- [x] 9.6 ACL hot reload: mutate ACL file on disk, assert new behavior within poll interval and old rules intact before swap; broken file keeps old rules
- [x] 9.7 Run the whole L2 suite under ASan to catch nocopy buffer lifetime bugs (use-after-free/leak)

## 10. System verification (L3, spec scenarios end to end)

- [x] 10.1 Prepare test fixtures: small file (≤ threshold), large file (> threshold, e.g. 64 MiB pattern file with `byte[i] = i % 251`), nested path
- [x] 10.2 curl range checks with **content verification**: download slices with `curl -r` and `cmp` them against `dd`-cut fragments of the fixture (verifies offset alignment, not just length); verify `Content-Range` totals
- [x] 10.3 curl checks for remaining semantics: 413 body content, HEAD sizes, invalid unit, traversal → 400, missing → 404, `Accept-Ranges` present on error responses
- [x] 10.4 Rate limit behavior against the real binary: sustained download approximates configured rate; disabled mode unrestricted
- [x] 10.5 Client IP checks over real connections: forged leftmost XFF ignored (blacklist the rightmost), missing XFF → peer addr; IPv6 client if available
- [x] 10.6 aria2 full loop (NOTE: aria2 unavailable without sudo; verified with an equivalent 4-thread cap-cycling downloader + sha256): `aria2c -x8` downloads the large file completely and correctly (validates capped-range + Content-Range total + parallel resume against a real download manager); checksum the result
- [x] 10.7 Resume simulation: partial download + `Range: bytes=<have>-` continues; offset == size → 416 confirms completion
- [x] 10.8 Concurrency/memory sanity: ~50 parallel capped-range downloads complete correctly; RSS stays bounded (~concurrency × cap)

## 11. Documentation

- [x] 11.1 README: purpose, build/run instructions (xmake), config file reference with defaults, ACL file format
- [x] 11.2 README: proxy requirement (XFF append, e.g. nginx `$proxy_add_x_forwarded_for`), trust_hops explanation, memory sizing note (max_connections × cap), single-instance rate-limit semantics. (Build now uses the xrepo `workflow` package directly, so the former "portable build fallback" note was removed.)
- [x] 11.3 README: how to run the tests (`xmake run unit-test`, `xmake run integration-test`, L3 script locations) and the ASan procedure
