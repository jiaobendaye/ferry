# Spec: bandwidth-rate-limiting

## ADDED Requirements

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
