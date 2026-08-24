## 1. Byte Quantity Parser

- [x] 1.1 Implement checked parsing for unsuffixed byte counts and the case-sensitive `B`, `KiB`, `MiB`, `GiB`, and `TiB` suffixes with optional separating whitespace.
- [x] 1.2 Apply byte-quantity parsing only to `cap_bytes` and `rate_bytes_per_sec`, retaining their existing result ranges and literal compatibility.

## 2. Regression Coverage

- [x] 2.1 Add unit tests for every supported suffix, optional whitespace, zero-rate disabling, and plain integer compatibility for both keys.
- [x] 2.2 Add unit tests for unknown or incorrectly cased suffixes, negative and fractional magnitudes, trailing input, multiplication overflow, and out-of-range results.

## 3. Documentation and Verification

- [x] 3.1 Document supported byte suffixes and examples in the English and Chinese configuration references.
- [x] 3.2 Run formatting checks, unit tests, integration tests, and strict OpenSpec validation.

## 4. Client MiB Arguments

- [x] 4.1 Parse `--chunk-size` and `--single-stream-limit` as integer MiB counts with checked conversion to internal bytes.
- [x] 4.2 Add unit coverage for MiB conversion, boundaries, malformed values, and multiplication overflow.
- [x] 4.3 Update client help, English and Chinese documentation, and repository-owned CLI invocations to use MiB values.
