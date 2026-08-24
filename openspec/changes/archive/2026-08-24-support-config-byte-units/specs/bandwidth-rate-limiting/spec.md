## MODIFIED Requirements

### Requirement: Configurable rate parameters
The rate (`rate_bytes_per_sec`), maximum wait (`max_wait_sec`), and idle-reclamation behavior SHALL be configurable. `rate_bytes_per_sec` SHALL accept either an unsuffixed decimal byte count or a non-negative integer followed by the case-sensitive binary byte suffix `B`, `KiB`, `MiB`, `GiB`, or `TiB`, with optional whitespace before the suffix. Invalid suffixes, multiplication overflow, and values outside the existing range SHALL cause startup to fail with a clear error identifying the key. A rate value that disables limiting (e.g. zero or unset) SHALL mean no bandwidth limiting is applied.

#### Scenario: Limiting disabled
- **WHEN** `rate_bytes_per_sec` is unset, `0`, or `0MiB`
- **THEN** all requests are served without token-bucket delay or 429

#### Scenario: Rate unit suffix honored
- **WHEN** the configuration sets `rate_bytes_per_sec = 10MiB`
- **THEN** the effective per-IP bandwidth rate is 10485760 bytes per second

#### Scenario: Invalid rate fails startup
- **WHEN** the configuration sets `rate_bytes_per_sec` to an unknown suffix, a negative magnitude, an overflowing quantity, or a result outside the allowed range
- **THEN** the server refuses to start with a clear error identifying the bad key
