# bandwidth-rate-limiting Specification

## Purpose

Limit per-client-IP bandwidth using a token bucket refilled at a configured rate: soft-shape mildly over-budget requests by delaying them with non-blocking timers, reject with `429` + `Retry-After` when the required wait exceeds a bound, and reclaim idle bucket entries so memory stays bounded.
## Requirements
### Requirement: Per-IP token-bucket bandwidth limit
The server SHALL limit bandwidth per resolved client IP using a token bucket refilled at the configured `rate_bytes_per_sec`. The limit applies to bandwidth only (there is no request-rate/QPS limit in this capability). Tokens SHALL be charged against the number of file-content bytes served in a `200` or `206` response; responses without file content (HEAD, 403, 404, 413, 416, 429) SHALL NOT consume tokens.

#### Scenario: Served bytes consume tokens
- **WHEN** a client with a fresh bucket receives a 206 response of N bytes
- **THEN** N tokens are deducted from that IP's bucket

#### Scenario: Error responses are free
- **WHEN** a client receives a 403, 404, or 416 response
- **THEN** no tokens are deducted

### Requirement: Soft shaping delays rather than rejects
When a request requires more tokens than are currently available but the required wait (`deficit / rate`) is less than or equal to the configured `max_wait_sec` (default 30 s), the server SHALL delay the request by that wait duration and then serve it normally (`200`/`206`). The delay SHALL be implemented without blocking any thread (Workflow series timer).

#### Scenario: Mildly over budget is delayed, not rejected
- **WHEN** a request needs slightly more tokens than available and the computed wait is ≤ `max_wait_sec`
- **THEN** the server waits the computed duration and responds `200`/`206` normally (no error status)

### Requirement: Bounded wait then 429
When the required wait for a request exceeds `max_wait_sec`, the server SHALL respond `429 Too Many Requests` with a `Retry-After` header indicating how long to wait, and SHALL NOT serve file content.

#### Scenario: Far over budget is rejected
- **WHEN** a request's computed wait exceeds `max_wait_sec`
- **THEN** the server responds `429` with a `Retry-After` header and no file content

### Requirement: Idle bucket reclamation
The server SHALL periodically reclaim token-bucket entries for IPs that have been idle beyond a reclamation threshold, so the per-IP map does not grow without bound.

#### Scenario: Idle entry reclaimed
- **WHEN** an IP has made no request beyond the idle threshold and a reclamation sweep runs
- **THEN** that IP's bucket entry is removed and a subsequent request starts from a fresh bucket

### Requirement: Configurable rate parameters
The rate (`rate_bytes_per_sec`), maximum wait (`max_wait_sec`), and idle-reclamation behavior SHALL be configurable. Invalid values SHALL cause startup to fail with a clear error. A rate value that disables limiting (e.g. zero or unset) SHALL mean no bandwidth limiting is applied.

#### Scenario: Limiting disabled
- **WHEN** `rate_bytes_per_sec` is unset or zero
- **THEN** all requests are served without token-bucket delay or 429

#### Scenario: Invalid rate fails startup
- **WHEN** the configuration sets `rate_bytes_per_sec` to a negative number
- **THEN** the server refuses to start with a clear error

### Requirement: Aggregate bandwidth limit
The server SHALL limit total bandwidth across all clients using an aggregate token bucket refilled at `rate_total_bps`, charging the number of file-content bytes served in `200`/`206` responses, with the same soft-shape-then-429 semantics as the per-IP limit (delay while the required wait is within `max_wait_sec`, otherwise `429` + `Retry-After`). Error responses without file content SHALL NOT consume aggregate tokens. A value of zero or unset SHALL disable the aggregate limit.

#### Scenario: Aggregate demand from many IPs is capped
- **WHEN** `rate_total_bps` is 1 MiB/s and 100 distinct IPs each request data concurrently while per-IP limits do not engage
- **THEN** the aggregate admitted bandwidth does not exceed 1 MiB/s plus burst capacity

#### Scenario: Error responses are free
- **WHEN** requests are answered with 429 or 404
- **THEN** no aggregate tokens are deducted

### Requirement: Composition of aggregate and per-IP bandwidth limits
When both `rate_total_bps` and `rate_bytes_per_sec` are enabled, a request SHALL be admitted only if both budgets allow it; the applied shaping delay SHALL be the maximum of the two computed waits; and both buckets SHALL be charged on admission. A rejection by either bucket SHALL result in a single `429` reply.

#### Scenario: Stricter budget decides the wait
- **WHEN** the per-IP bucket computes a 3 s wait and the aggregate bucket computes a 1 s wait for the same request
- **THEN** the request is delayed approximately 3 s, both buckets are charged, and the response is 200/206

#### Scenario: Either bucket can reject
- **WHEN** the aggregate bucket's required wait exceeds `max_wait_sec` while the per-IP bucket has budget
- **THEN** the request receives a single `429` with `Retry-After`

### Requirement: Aggregate limit configuration
The key `rate_total_bps` SHALL be configurable in the server configuration file, defaulting to off (zero/unset). Invalid values (negative numbers) SHALL cause startup to fail with a clear error naming the key. The effective aggregate limit SHALL be included in the startup log line.

#### Scenario: Aggregate limiting disabled by default
- **WHEN** `rate_total_bps` is unset
- **THEN** bandwidth limiting behaves exactly as the per-IP-only configuration

#### Scenario: Invalid value fails startup
- **WHEN** the configuration sets `rate_total_bps` to a negative number
- **THEN** the server refuses to start with an error naming `rate_total_bps`
