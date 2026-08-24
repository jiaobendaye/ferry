## MODIFIED Requirements

### Requirement: Command-line interface
The client SHALL accept a single URL plus `-o/--output` (default: URL basename), `-j/--jobs` (default 4), `--chunk-size` (integer MiB, default 8), `--checksum`, `--no-verify`, `--receive-timeout` (default 60 s), `--single-stream-limit` (integer MiB, default 256), and `-q/--quiet`. The client SHALL convert the two MiB quantities to bytes internally and reject malformed, negative, fractional, suffixed, or overflowing values with a clear error.

#### Scenario: Output defaults to basename
- **WHEN** invoked as `ferry-client http://host/dir/big.bin` without `-o`
- **THEN** the output file is `big.bin` in the working directory

#### Scenario: Client MiB quantities are converted to bytes
- **WHEN** invoked with `--chunk-size 8 --single-stream-limit 256`
- **THEN** the effective chunk size is 8388608 bytes and the effective single-stream limit is 268435456 bytes

#### Scenario: Invalid jobs rejected
- **WHEN** invoked with `-j 0`
- **THEN** the client refuses to start with an error

#### Scenario: Invalid MiB quantity rejected
- **WHEN** a client MiB option is fractional, suffixed, negative, or overflows when converted to bytes
- **THEN** the client refuses to start with an error identifying the option
