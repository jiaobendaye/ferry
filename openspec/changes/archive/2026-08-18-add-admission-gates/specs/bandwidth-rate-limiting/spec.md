## ADDED Requirements

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
