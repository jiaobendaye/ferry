# Design: add-admission-gates

## Context

ferry-server today has two load controls: Workflow's `max_connections` (TCP connection cap, set in `main.cc`) and a per-IP bandwidth token bucket (`RateLimiter`), wired by hand into `Handler::process`: reserve → on rejection reply 429 → otherwise push a timer (soft shaping) and a `pread` task into the Workflow series. The handler is a single linear function: method check → client IP → ACL → path safety → stat → HEAD shortcut → Range decision → rate limiter → async file read. Responses are buffered whole before send (Workflow's server model), so the peak-memory term is `in-flight responses × cap_bytes`, but nothing bounds in-flight *requests* — only sockets.

Constraints carried into this design:

- **No architectural change** (user decision): keep the buffered-response / delay-then-burst model; no streaming send.
- **Workflow task model**: gates must decide synchronously inside `process()`; waits are timers in the series; release/stats hooks live in the server-task callback, which today is only set on the file-serving path.
- **House patterns**: injectable clock with no sleeps in unit tests; config keys default to "off" and fail startup loudly when invalid; three-layer testing (L1 unit / L2 integration / L3 system scripts).

Stakeholders: operators deploying ferry-server behind proxies (protection + sizing), ferry-client users (their `-j N` workers interact with per-IP gates), and future maintainers adding further gates.

## Goals / Non-Goals

**Goals:**

- Global (server-protection) caps for QPS, in-flight requests, and bandwidth; per-IP (fairness) caps for the same three dimensions.
- A gate abstraction (`Gate` / `GateChain`) so that adding, ordering, and testing gates does not grow `Handler::process` linearly; the existing per-IP bandwidth limiter is refactored into the same abstraction.
- No leaks, no over-grant under concurrency; limiter state (per-IP maps) bounded even under IP-rotation floods.
- Observable operation: counters, gauges, periodic stats line, on-demand dump.
- Byte-identical behavior when no new key is configured.

**Non-Goals:**

- Per-CIDR (subnet) rate limiting — noted as a future middle layer between per-IP and global; rejected for now because global-first ordering plus sweep already bounds state and totals.
- Distributed / multi-instance limiting (single-instance design, as with the existing limiter).
- True send-time throttling (would require streaming; out by constraint).
- HTTP `/stats` endpoint (unauthenticated state exposure + path-safety special case; stderr + signal suffice, and upstream proxies have their own metrics).
- Client changes (ferry-client already handles 429 and retries 5xx).

## Decisions

### D1: Gate chain, not next()-style middleware

`Gate` has a synchronous `check(ctx) -> Verdict` (pass / delay / reject); a `GateChain` runner iterates gates, composes verdicts, and collects release obligations. Rejected: Express-style `next()` nesting. Two structural mismatches: (1) data dependency splits the pipeline — QPS/concurrency gates need only the client IP, bandwidth gates need the response length which only exists after the Range decision — so no single nested chain can express it; (2) the asynchronous tail (timer + pread in the Workflow series) is not a function call that middleware can wrap, and releases happen later in the server-task callback, outside any stack. The gate chain keeps every benefit that matters here (pluggable, independently testable, cross-cutting stats hook) without pretending to onion-wrap an async task graph.

### D2: Two gate kinds map to two resource primitives

- **TokenBucketGate** (QPS, bandwidth): `reserve(key, units)` → delay (soft shape) or 429 when the wait exceeds `max_wait_sec`. Units are chosen by the gate: `1` per request for QPS, `ctx.bytes` for bandwidth.
- **SemaphoreGate** (concurrency): `try_acquire(key)` → ok with a release closure, or immediate 503. No queueing: queueing would require deciding whether *waiting* requests occupy slots; hard rejection matches `limit_conn` semantics and keeps one invariant (`inflight ≤ cap` strictly).

Rejection codes keep their RFC roles: 429 = "you are over rate" (client-specific), 503 = "server is overloaded" (global condition).

### D3: Two chains split at the Range decision; global gates first within each

```
IP → ACL → pre-chain ──▶ path/stat → HEAD? → Range ──▶ post-chain ──▶ serve
         1. qps_total (429)                       5. rate_total_bps (429)
         2. max_inflight (503)                    6. rate_bytes_per_sec (429)
         3. qps_per_ip (429)
         4. max_inflight_per_ip (503)
```

Global-first ordering is not stylistic: per-IP gates create per-IP state (bucket map entries, counter map entries) on first touch. A rotation flood rejected by the global gates never reaches per-IP state, so steady-state map size is bounded by `qps_total × sweep window`. Alternative considered (per-IP first, for earlier fairness decisions) rejected: fairness is meaningless once the server is over its global envelope, and the state-bounding property would be lost.

### D4: Delay composition is max, not sum

All shaper gates charge at check time; each returns the wait until its own budget is available. Since all charges land at the same instant, the request is ready when the *strictest* bucket is ready: `delay = max(waits)`. Summing would over-penalize. Alternative (sequential re-checks) adds lock traffic for no semantic gain.

### D5: RAII release list + unified server-task callback

The chain runner builds a `ReleaseList` (vector of closures). On mid-chain rejection it is destroyed at scope exit, releasing acquired semaphores in reverse order — no error path can leak. On success the list is moved into a heap `RequestState` anchored to `server_task->user_data`; `server_task->set_callback` is installed **at the top of `process()` for every path** (fixing today's gap where early-return paths set no callback) and is the single point that: releases gates, frees `FileContext` if present, and records stats from `resp->get_status_code()`. Alternative (release inline in each rejection branch) is exactly the bug-prone pattern the abstraction exists to eliminate.

Known subtlety to verify in implementation: the Workflow server-task callback must fire for synchronous early-return paths as well; L2 tests assert this indirectly (a leaked slot would drive inflight to the cap and wedge the server, which the thundering-herd test detects).

### D6: Extract `TokenBucket` from `RateLimiter`

`TokenBucket` = single bucket, injectable clock, `reserve(tokens) -> Verdict`. `RateLimiter` becomes keyed map + sweep over `TokenBucket` instances; global gates hold a bare `TokenBucket` (no fake `"__global__"` key in a per-IP map). Existing `RateLimiter` unit tests (fake clock, no sleeps) migrate to `TokenBucket` nearly verbatim; map/sweep tests stay on `RateLimiter`.

### D7: Per-IP concurrency counters erase-on-zero; no sweep needed

Unlike token buckets (which persist with idle tokens), semaphore counters return to zero naturally. `release()` decrements under the mutex and erases the entry at zero — the map cannot accumulate idle entries, so no periodic sweep is required. Races between erase-on-zero and a concurrent acquire for the same IP serialize on the mutex; either interleaving is correct (acquire recreates the entry).

### D8: QPS gates soft-shape like the bandwidth gate

QPS overage delays (wait = deficit/qps) up to `max_wait_sec`, then 429 — reusing the bandwidth gate's semantics and `max_wait_sec` knob rather than adding a second wait config. Rationale: consistent behavior, full code/test reuse, smooths bursts instead of chopping them, and ferry-client's 429 handling is already proven. Hard-reject QPS was considered and rejected as an unjustified second semantic.

### D9: Charged-then-rejected waste is accepted and documented

If a request passes `qps_total` but is rejected by `max_inflight`, the QPS tokens stay charged. Mitigation is the gate order (D3): the cheapest most-likely-rejecting global gates run first, so the window is small; and rejected requests do consume real CPU, so charging them is defensible. Alternatives (two-phase query-then-commit across gates) add race surface to an inherently approximate mechanism for negligible gain.

### D10: Stats are recorded at the unified callback + gate names key rejection counters

`Stats` holds atomic counters: requests, status classes (2xx/404/other-4xx), per-gate rejection counts keyed by `gate->name()`, bytes served, `inflight` gauge and peak. Adding a gate automatically yields its own rejection counter — the observability hook attaches to the chain runner, not to each gate. The periodic printer reuses the existing `arm_periodic` self-rescheduling timer in `main.cc`. SIGUSR1 only sets an `atomic<bool>` (signal-handler-safe); the 1 s tick observes it and dumps immediately. Line format is single-line `key=value` for grep/awk friendliness, with per-interval deltas (`reqs=N (+dN)`) so the line reads as rates directly.

### D11: Testing strategy — fake time for correctness, real time for throughput

- **L1**: thread-hammer tests on `TokenBucket`/`ConcurrencyLimiter` with real threads but fake clock — deterministic proofs of "never over-grant" and "no leak" (time only advances when the test advances it). Run under TSan (new build mode alongside the existing `--asan`); races in the keyed maps are TSan's core competency.
- **L2**: real server with tiny limits (`qps_total=100`, `max_inflight=10`) + an in-process WFHttpClient hammer. Assertions are bound-based (served ≤ cap + burst tolerance; ratios under saturation ≈ expected fraction) with ≥ 5 s windows, except the thundering-herd test (exact pass count, clock-independent). Post-storm assertions: `inflight == 0`, per-IP maps empty, bucket map below its bound after an IP-rotation flood via forged XFF (direct connection + client-supplied XFF exercises identity rotation without a real botnet).
- **L3** (`tests/stress/`, opt-in, not CI): vegeta constant-rate against QPS caps, wrk/hey max-concurrency against inflight caps, ferry-client `-j 32` closed loop under per-IP pressure (verifies the ecosystem degrades gracefully and finishes), and a 10-minute shaping-backlog soak observing stable stats lines.

### D12: Keep the single-mutex keyed maps until stress data says otherwise

`RateLimiter` uses one mutex for the whole map; the new per-IP semaphore map does the same. Sharding by key hash is the known fix if lock contention proves limiting, but it is deliberately deferred: the L1 hammer measures reserve ops/sec and the L3 runs measure gates-on vs gates-off throughput, so the decision becomes data-driven instead of speculative.

## Risks / Trade-offs

- **Unified-callback assumption on early-return paths** (D5) → L2 integration test exercises every rejection path under load; a leaked slot wedges the server visibly (everything 503s), so failure mode is loud, not silent.
- **Waiting (shaped) requests occupy inflight slots and connections** → intended and correct (buffered bytes are still bounded), but under rate+heavy load the server settles into "busy waiting" where most slots sleep in timers. Documented in README deployment notes; the L3 soak test asserts stability of this regime.
- **Stats atomics on the hot path** → each is one atomic increment per request; negligible vs file I/O. Rejection counters use a small fixed array indexed by gate, not a map lookup.
- **503 + Retry-After: 1 for clients that don't honor it** → ferry-client honors it; third-party clients that ignore it simply re-collide with the gate. Acceptable; Retry-After is advice, not enforcement.
- **Behavioral surface growth in config** (six new keys) → all default off; startup prints effective config (existing behavior extended), so misconfiguration is visible at boot; invalid values fail startup per house pattern.
- **TSan + Workflow compatibility unknown** → if TSan reports Workflow-internal noise, scope the TSan build to the L1 hammer binary only (it links the limiter code, not the server).

## Migration Plan

No migration: all gates default to off, so upgrading with an unchanged `server.conf` is a no-op behaviorally. Rollback = revert the binary. Suggested enablement order for operators: `max_inflight` first (memory bound), then `qps_total` (flood bound), then per-IP fairness knobs; observe via the stats line between steps. README/README.zh-CN gain the config rows, a protection-vs-fairness note, and the new client coupling (a high-`-j` client shares one IP's QPS/concurrency budget).

## Open Questions

None blocking. The three micro-decisions surfaced during exploration are resolved here: `max_wait_sec` reused for QPS shaping (D8), 503 `Retry-After` hard-coded to 1 s (D2), charged-then-rejected waste accepted (D9).
