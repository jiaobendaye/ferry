## ADDED Requirements

### Requirement: Global request-rate (QPS) limit
The server SHALL limit the aggregate request rate across all clients using a token bucket refilled at `qps_total` requests per second. Each request past method/ACL validation SHALL consume one token. When the required wait (deficit / qps_total) is within `max_wait_sec` the request SHALL be delayed by a non-blocking timer and then processed normally; when it exceeds `max_wait_sec` the server SHALL respond `429` with a `Retry-After` header and no file content. A value of zero or unset SHALL disable the gate.

#### Scenario: Aggregate flood is capped regardless of client count
- **WHEN** `qps_total` is 100 and 10,000 distinct client IPs each send requests within one second
- **THEN** the number of requests admitted in that second does not exceed 100 plus the configured burst capacity, no matter how the requests are distributed across IPs

#### Scenario: Over-rate request is delayed, then served
- **WHEN** a request's computed QPS wait is 200 ms and within `max_wait_sec`
- **THEN** the response arrives approximately 200 ms later with the normal status (no 429)

#### Scenario: Far over rate is rejected
- **WHEN** the computed QPS wait exceeds `max_wait_sec`
- **THEN** the server responds `429` with `Retry-After` and serves no file content

### Requirement: Per-IP request-rate (QPS) limit
The server SHALL limit the request rate per resolved client IP using a token bucket refilled at `qps_per_ip` requests per second, consuming one token per request, with the same soft-shape-then-429 semantics as the global QPS gate. Per-IP QPS state for idle IPs SHALL be reclaimed so the map does not grow without bound. A value of zero or unset SHALL disable the gate.

#### Scenario: One client cannot exceed its QPS quota
- **WHEN** `qps_per_ip` is 10 and a single client sends 50 requests in one second while global limits do not engage
- **THEN** at most 10 plus burst capacity are admitted and the remainder receive 429 or are delayed

#### Scenario: Idle per-IP QPS state reclaimed
- **WHEN** an IP has been idle beyond the reclamation threshold and a sweep runs
- **THEN** its QPS bucket entry is removed

### Requirement: Global concurrency limit
The server SHALL bound the number of in-flight requests (admitted but not yet completed) at `max_inflight`. Admission SHALL be hard: when the limit is reached the server SHALL respond `503` with `Retry-After: 1` and no file content, without queueing. A slot SHALL be released when the response completes, on every path including asynchronous file serving and all error replies. The invariant `in-flight <= max_inflight` SHALL hold at all times. A value of zero or unset SHALL disable the gate.

#### Scenario: Thundering herd admits exactly the cap
- **WHEN** `max_inflight` is 50 and 1000 requests arrive simultaneously while responses are slow
- **THEN** exactly 50 are admitted and 950 receive `503` with `Retry-After`

#### Scenario: Slots released on every path
- **WHEN** a burst of requests completes, spanning file responses, 404s, and 429s
- **THEN** the in-flight count returns to exactly zero and subsequent requests are admitted up to the cap again

#### Scenario: Memory bound follows the cap
- **WHEN** `max_inflight` is set
- **THEN** peak buffered response memory is bounded by `max_inflight × cap_bytes`

### Requirement: Per-IP concurrency limit
The server SHALL bound the number of in-flight requests per resolved client IP at `max_inflight_per_ip`, rejecting excess with `503` and releasing per-IP slots at response completion. Per-IP counter entries that return to zero SHALL be removed (no periodic sweep required). A value of zero or unset SHALL disable the gate.

#### Scenario: One client cannot occupy all slots
- **WHEN** `max_inflight_per_ip` is 4 and a client opens 8 concurrent requests while global limits do not engage
- **THEN** at most 4 are in flight for that IP and the rest receive `503`

#### Scenario: Counter entry removed at zero
- **WHEN** an IP's last in-flight request completes
- **THEN** its per-IP concurrency entry no longer occupies the map

### Requirement: Gate ordering and state bounding
Global gates SHALL be evaluated before per-IP gates, and QPS/concurrency gates SHALL be evaluated before path resolution, so that requests rejected by global gates do not create per-IP limiter state. Bandwidth gates SHALL be evaluated after the Range decision, when the response length is known.

#### Scenario: IP-rotation flood does not grow per-IP state
- **WHEN** `qps_total` is set and a flood of requests arrives with continuously rotating client IPs
- **THEN** requests beyond the global cap are rejected before per-IP state is created, and the per-IP map size stays bounded by the global admission rate times the reclamation window

### Requirement: Delay composition across shaper gates
When multiple token-bucket gates admit a request with nonzero waits, the server SHALL delay by the maximum of the waits, not their sum, and then serve the request normally.

#### Scenario: Two shapers admit with different waits
- **WHEN** the per-IP bandwidth gate computes a 5 s wait and the global bandwidth gate computes a 2 s wait for the same request
- **THEN** the request is delayed approximately 5 s total and served once

### Requirement: Resource rollback on rejection and release at completion
When a request is rejected by any gate, all resources acquired from earlier gates in the chain SHALL be released before the rejection reply. When a request passes all gates, acquired resources SHALL be released exactly once, at response completion, via the unified request-completion hook covering both synchronous and asynchronous paths.

#### Scenario: Mid-chain rejection rolls back earlier acquisitions
- **WHEN** a request passes the global QPS gate but is rejected by the global concurrency gate
- **THEN** the concurrency gate holds no leaked slot and the in-flight gauge is unchanged after the reply

#### Scenario: Async completion releases exactly once
- **WHEN** an admitted file request completes after shaping and pread
- **THEN** its concurrency slot(s) are released exactly once and the in-flight gauge reflects it

### Requirement: HEAD requests and admission gates
HEAD requests SHALL be admitted or rejected by QPS and concurrency gates like any other request, and SHALL NOT consume bandwidth tokens (consistent with the existing bandwidth capability).

#### Scenario: HEAD counts for QPS and concurrency
- **WHEN** a client sends HEAD requests at a rate above `qps_per_ip`
- **THEN** the excess HEAD requests are delayed or rejected with 429 like GET requests

### Requirement: Gate configuration
The keys `qps_total`, `qps_per_ip`, `max_inflight`, and `max_inflight_per_ip` SHALL be configurable in the server configuration file, each defaulting to off (zero/unset). Invalid values (negative numbers) SHALL cause startup to fail with a clear error naming the key. The effective gate configuration SHALL be included in the startup log line.

#### Scenario: All gates off by default
- **WHEN** the configuration file sets none of the new keys
- **THEN** server behavior is identical to a build without admission gates

#### Scenario: Invalid value fails startup
- **WHEN** the configuration sets `max_inflight` to a negative number
- **THEN** the server refuses to start with an error naming `max_inflight`
