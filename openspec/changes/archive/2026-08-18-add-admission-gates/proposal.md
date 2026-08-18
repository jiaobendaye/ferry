# Proposal: add-admission-gates

## Why

ferry-server's only load controls today are `max_connections` (TCP-level) and a per-IP bandwidth token bucket. Per-IP limits provide fairness, not protection: total demand equals per-IP quota times the number of distinct IPs, and the IP count is attacker-controlled (botnets, IPv6 /64 rotation). Nothing bounds request rate, in-flight request count, or aggregate bandwidth, so a distributed flood can exhaust CPU, memory (in-flight responses are buffered at up to `cap_bytes` each), or uplink while every single IP stays "within quota". The limiter's own per-IP map is also unbounded against IP-rotation floods. The server additionally runs silent after startup — there is no way to observe whether limits are engaging.

## What Changes

- **Global admission gates** (server protection, identity-independent): `qps_total` (whole-server request rate), `max_inflight` (whole-server in-flight request count — bounds peak memory at `max_inflight × cap_bytes`), `rate_total_bps` (whole-server bandwidth).
- **Per-IP admission gates** (fairness): `qps_per_ip`, `max_inflight_per_ip`. The existing per-IP bandwidth limiter stays and joins the same framework.
- **Gate-chain abstraction instead of inline checks**: a `Gate` interface with two concrete kinds — token-bucket gates (soft-shape then `429`) and semaphore gates (hard-reject `503`) — run by a `GateChain` runner that composes verdicts (delays combine by max), rolls back acquired resources via an RAII release list on mid-chain rejection, and hands the release list to the unified server-task callback on success. The existing per-IP bandwidth limiter is refactored into this abstraction (its hand-wired reserve/timer/429 code in the handler is removed), which validates the abstraction against an existing use case.
- **Two-chain pipeline split** driven by data dependency: a pre-chain (needs only the client IP: `qps_total` → `max_inflight` → `qps_per_ip` → `max_inflight_per_ip`) runs before path resolution; a post-chain (needs the response byte count: `rate_total_bps` → `rate_bytes_per_sec`) runs after the Range decision. Global gates precede per-IP gates so floods are rejected before per-IP state (bucket map, counter map) is created — bounding limiter memory as a side effect.
- **Token-bucket extraction**: the single-bucket core is extracted from `RateLimiter` (injectable clock preserved) so global gates hold a bare bucket rather than a keyed map with a fake key.
- **Rejection semantics**: QPS/bandwidth gates soft-shape (delay ≤ `max_wait_sec`) then `429 + Retry-After`; concurrency gates hard-reject with `503 + Retry-After: 1`. Client compatibility: ferry-client already honors 429 via `max(Retry-After, backoff)` and retries 5xx — no client changes.
- **Observability**: a shared `Stats` structure (atomic counters) recorded at the unified callback (final status) and at each gate rejection (keyed by gate name); a periodic one-line `[stats]` summary on stderr every `stats_interval_sec`; `SIGUSR1` triggers an on-demand dump.
- **Stress-testing layers** (per user requirement): L1 thread-hammer tests against the limiter primitives with fake clocks and TSan (deterministic over-grant/leak proofs); L2 behavioral tests with tiny configured limits and a synthetic hammer client asserting bounds on served rates, exact thundering-herd pass counts, and zero leaked state after storms; L3 opt-in scripts (`tests/stress/`) using external load tools and a ferry-client closed loop, kept out of CI.
- All new gates default to off (`0`); behavior with an unmodified config is byte-identical to today. No breaking changes.

## Capabilities

### New Capabilities

- `admission-gates`: request-rate and concurrency admission control — global and per-IP QPS token-bucket gates, global and per-IP concurrency semaphore gates, gate ordering, delay composition, resource rollback on mid-chain rejection, release at request completion, and 429/503 rejection semantics.
- `server-observability`: runtime visibility into server operation — request/status/rejection counters per gate, current and peak in-flight gauges, served bytes, periodic stderr stats line, and on-demand SIGUSR1 dump.

### Modified Capabilities

- `bandwidth-rate-limiting`: adds an aggregate (whole-server) bandwidth cap alongside the existing per-IP limit, configurable via `rate_total_bps`, enforced in the same soft-shape-then-429 manner and composed with the per-IP limit (a request must pass both).

## Impact

- **Server code**: new `token_bucket`, `concurrency_limiter`, `gate`/`gates`, `stats` modules; `rate_limiter` refactored to a keyed map over the extracted bucket; `handler.cc` restructured around the two chains and a unified server-task callback (which also fixes the current gap where early-return paths set no task callback); `config` gains six keys; `main.cc` wires chains, stats, the stats periodic task, and SIGUSR1.
- **Tests**: `tests/unit` (bucket/semaphore/chain/stats + thread hammers), `tests/integration` (gate behavior end-to-end with small limits), `tests/stress` (new, opt-in L3 scripts). ASan runs continue; a TSan build mode is added for the hammer tests.
- **Client**: none (429 and 5xx handling already exist); README's "couplings" note gains the per-IP concurrency/QPS interaction (a high `-j` client can hit its own IP's gates and must back off).
- **Docs**: README / README.zh-CN gain the new config keys, a protection-vs-fairness deployment note (global gates protect, per-IP gates divide), and stats-line documentation.
- **Operational**: peak memory becomes strictly `max_inflight × cap_bytes` when configured — a tighter and simpler sizing story than the connection-based estimate.
