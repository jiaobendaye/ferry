# Tasks: add-admission-gates

## 1. Core primitives (parallelizable; no cross-dependencies)

- [x] 1.1 Extract `TokenBucket` (single bucket, injectable clock, `reserve(tokens) -> Verdict{rejected, wait}`, charge-on-grant, no-state-change-on-reject) from `rate_limiter.{h,cc}`; migrate the existing bucket-behavior unit tests to it (fake clock, no sleeps)
- [x] 1.2 Refactor `RateLimiter` into a keyed map of `TokenBucket` + sweep over the extracted bucket; keep its existing map/sweep unit tests passing
- [x] 1.3 Implement `ConcurrencyLimiter` (`server/concurrency_limiter.{h,cc}`): global mode (atomic counter) and keyed mode (mutex + map, erase-on-zero), `try_acquire(key?) -> bool` / `release(key?)`, injectable for tests; unit tests: cap enforcement, release-on-zero erases entry, no leak after balanced acquire/release
- [x] 1.4 Implement `Stats` (`server/stats.{h,cc}`): atomic counters (requests, 2xx/404/other-4xx/5xx, per-gate rejection counts over a fixed gate-index array, bytes served) and in-flight gauge + peak; unit tests including the reconciliation invariant (total == sum of statuses)

## 2. Gate abstraction

- [x] 2.1 Implement `Gate` interface, `GateCtx` (ip_key, bytes, resp), `GateVerdict` (rejected/status/retry_after/delay/release closure), `ReleaseList` (RAII reverse-order release, move-able ownership) in `server/gate.{h,cc}`; unit tests for ReleaseList rollback-on-destroy and move semantics
- [x] 2.2 Implement `GateChain` runner: iterate gates, reject-short-circuit with auto-rollback, delay composition by max, release-list handoff on success; record per-gate rejections into `Stats`; unit tests with stub gates (reject mid-chain rolls back earlier acquisitions; delays compose by max)
- [x] 2.3 Implement `TokenBucketGate` (wraps `TokenBucket` for global keys or `RateLimiter` for per-IP keys; units chosen by gate: 1 for QPS, `ctx.bytes` for bandwidth; 429 + Retry-After on rejection) and `SemaphoreGate` (wraps `ConcurrencyLimiter`; 503 + Retry-After: 1) in `server/gates.{h,cc}`; unit tests with fake clocks
- [x] 2.4 Add a chain factory that builds pre-chain (`qps_total` → `max_inflight` → `qps_per_ip` → `max_inflight_per_ip`) and post-chain (`rate_total_bps` → `rate_bytes_per_sec`) from `ServerConfig`, omitting disabled gates; unit test that empty config produces empty chains

## 3. Configuration

- [x] 3.1 Add keys `qps_total`, `qps_per_ip`, `max_inflight`, `max_inflight_per_ip`, `rate_total_bps`, `stats_interval_sec` to `ServerConfig` and `load_config` (defaults off/zero; negative values throw naming the key); unit tests per key: default, valid, invalid

## 4. Handler integration

- [x] 4.1 Restructure `Handler::process`: install the unified `server_task` callback at the top for every path (releases gates, frees `FileContext` if any, records status/bytes into `Stats`), run pre-chain after ACL, run post-chain after the Range decision sets `ctx.bytes`; remove the hand-wired `RateLimiter` reserve/timer/429 block (replaced by post-chain); push a single timer with the composed delay
- [x] 4.2 Verify HEAD semantics: HEAD passes through pre-chain gates but short-circuits before the post-chain (no bandwidth charge); extend existing handler unit/integration tests to cover it
- [x] 4.3 Integration tests (L2, existing suite extended): every synchronous error path (405/400/403/404/413/416) and every gate rejection releases resources — asserted via the in-flight gauge returning to zero after a mixed burst

## 5. Server wiring

- [x] 5.1 In `main.cc`: build chains from config, pass `Stats` to handler and chains; construct the aggregate `TokenBucket` for `rate_total_bps`
- [x] 5.2 Add the periodic stats printer as a third `arm_periodic` task gated on `stats_interval_sec > 0`, emitting the single-line `key=value` summary (totals + per-interval deltas, per-gate rejections, inflight current/peak, buckets active, served bytes)
- [x] 5.3 Add SIGUSR1: handler sets an atomic flag; the periodic 1 s tick observes it and dumps immediately (works with periodic stats disabled — arm a 1 s tick task whenever SIGUSR1 handling is enabled)
- [x] 5.4 Extend the startup log line with the effective gate configuration (each gate's limit or "off")

## 6. L2 gate-behavior tests (tiny limits, synthetic hammer)

- [x] 6.1 Add an in-process hammer client (WFHttpClient-based, in the integration-test target) that drives configurable concurrency/request counts against the running test server and collects status-code tallies
- [x] 6.2 Thundering-herd test: `max_inflight=10`, slow file, 200 simultaneous requests → exactly 10 admitted, 190 × 503, gauge drains to zero
- [x] 6.3 QPS saturation test: `qps_total=100`, hammer at ~2× for ≥ 5 s → admitted rate ≤ 100 + burst tolerance; per-gate rejection counter for qps_total nonzero; totals reconcile
- [x] 6.4 Per-IP fairness test: `qps_per_ip`/`max_inflight_per_ip` small, one hammer client over its quota while a second client stays fully admitted
- [x] 6.5 Aggregate bandwidth test: `rate_total_bps` tiny, multiple forged-IP clients (XFF) → aggregate admitted bytes/s bounded; composition with per-IP limit verified (max-of-waits, single 429)
- [x] 6.6 IP-rotation flood test: forged rotating XFF identities against `qps_total` → per-IP bucket-map size stays bounded by admission rate × sweep window (assert via limiter `size()`)

## 7. Stress tests and sanitizer support

- [x] 7.1 Add a TSan build mode (`xmake f --tsan=y`) alongside the existing `--asan` mode, wired for the unit-test binary
- [x] 7.2 L1 thread-hammer tests: N threads × M iterations on `TokenBucket` and `ConcurrencyLimiter` with fake clocks — assert no over-grant (admitted ≤ theoretical bound exactly), no leak (gauges/maps empty at end); run clean under TSan
- [x] 7.3 Create `tests/stress/` with opt-in scripts (documented as not-CI): `run_stress_qps.sh` (vegeta or hey constant-rate against `qps_total`, assert ~50% admitted at 2× limit), `run_stress_conc.sh` (wrk/hey max-concurrency vs `max_inflight`), `run_stress_client.sh` (ferry-client `-j 32` closed loop under per-IP limits — completes via backoff, no hang), `run_stress_soak.sh` (10-minute shaping-backlog soak asserting stable stats lines)
- [x] 7.4 Gates-overhead measurement task in `tests/stress/`: throughput with all gates off vs on, reported for the single-mutex-vs-sharding decision (D12)

## 8. Documentation

- [x] 8.1 README: config-table rows for the six new keys; deployment section on protection-vs-fairness (global gates protect, per-IP gates divide); stats-line reference incl. SIGUSR1; new client coupling note (one client's workers share its IP's QPS/concurrency budget); memory-sizing note updated to `max_inflight × cap_bytes` when configured
- [x] 8.2 README.zh-CN: mirror all 8.1 changes

## 9. Final verification

- [x] 9.1 Full matrix: `xmake` clean build; unit-test + integration-test under release, ASan, and TSan (unit); L3 server + client scripts; stress scripts smoke-run
- [x] 9.2 Behavior-parity check: run the existing L3 suites against a config with all new keys unset and diff server behavior against the pre-change baseline (no new headers, statuses, or timing changes)
- [x] 9.3 `openspec validate` on the change; update spec deltas into `openspec/specs/` via archive when implementation is accepted
