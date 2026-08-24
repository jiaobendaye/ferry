## MODIFIED Requirements

### Requirement: Configurable serving parameters
The cap (`cap_bytes`), size threshold (`size_threshold_bytes`), listen port, and root directory SHALL be configurable via the server configuration file. `cap_bytes` SHALL accept either an unsuffixed decimal byte count or a non-negative integer followed by the case-sensitive binary byte suffix `B`, `KiB`, `MiB`, `GiB`, or `TiB`, with optional whitespace before the suffix. Missing keys SHALL fall back to documented defaults (cap = 8 MiB; threshold = cap). Invalid suffixes, multiplication overflow, and values outside the existing range SHALL cause startup to fail with a clear error identifying the key.

#### Scenario: Custom cap honored
- **WHEN** the server is configured with `cap_bytes = 1048576` and a client requests `Range: bytes=0-` for a 10 MiB file
- **THEN** the response contains exactly 1 MiB of data with `Content-Range` total of 10 MiB

#### Scenario: Cap unit suffix honored
- **WHEN** the server is configured with `cap_bytes = 8MiB`
- **THEN** the effective response cap is 8388608 bytes

#### Scenario: Invalid config fails startup
- **WHEN** the configuration file sets `cap_bytes` to an unknown suffix, a negative magnitude, an overflowing quantity, or a result outside the allowed range
- **THEN** the server refuses to start and prints an error identifying the bad key
