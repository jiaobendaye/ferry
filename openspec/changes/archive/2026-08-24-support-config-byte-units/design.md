## Context

The flat server configuration loader trims values and parses numeric keys with `strtoll`, requiring callers to precompute byte counts. The client likewise accepts raw byte counts for `--chunk-size` and `--single-stream-limit`, despite documenting their defaults in MiB. Server configuration and client arguments are parsed once at startup, so explicit and unambiguous unit semantics are more important than parser flexibility.

## Goals / Non-Goals

**Goals:**

- Accept plain decimal byte counts and integer magnitudes with IEC binary byte suffixes for the two selected keys.
- Detect malformed values, unsupported suffixes, multiplication overflow, and final range violations before startup.
- Keep errors tied to the offending configuration key and value.
- Accept the client chunk size and single-stream limit as integer MiB counts and convert them safely to internal bytes.

**Non-Goals:**

- Supporting arithmetic expressions, fractional magnitudes, variables, functions, or hexadecimal literals.
- Supporting ambiguous SI suffixes such as `KB`, `MB`, or lowercase bit units.
- Enabling suffixes for other numeric server configuration keys or client CLI values.
- Changing defaults or runtime limiter/Range behavior.

## Decisions

### Use strict IEC binary byte suffixes

Recognize case-sensitive `B`, `KiB`, `MiB`, `GiB`, and `TiB`, with multipliers from 1 through 1024^4. Permit optional ASCII whitespace between an unsigned decimal magnitude and its suffix. An absent suffix retains the existing byte interpretation.

Strict IEC names avoid the 1000-versus-1024 ambiguity of `KB` and the bytes-versus-bits ambiguity of `Mb`. Fractional values are excluded so all configuration remains exact integer arithmetic.

### Share one parser but opt in only the requested keys

Add a byte-quantity range parser and call it only for `cap_bytes` and `rate_bytes_per_sec`. Existing `parse_range` remains unchanged for ports, thresholds, aggregate limits, gate settings, and other numeric values.

For `rate_bytes_per_sec`, the parsed quantity is the number of bytes allowed per second; the suffix itself therefore remains a byte suffix rather than including `/s`.

### Check scaling before applying existing ranges

Parse the magnitude as a non-negative `long long`, check multiplication by the unit factor for overflow, then pass the scaled byte value through each key's existing range. This preserves `cap_bytes >= 1`, permits a zero rate to disable limiting, and retains the 1 TiB upper bounds.

### Treat client option values as integer MiB counts

`--chunk-size` and `--single-stream-limit` accept an unsuffixed decimal integer whose unit is MiB. The parser checks multiplication by 1024^2 before storing the internal byte count. `--chunk-size` remains positive and `--single-stream-limit` continues to allow zero; their defaults remain 8 MiB and 256 MiB. Fractions and unit suffixes are rejected so the CLI has one concise input form, such as `--chunk-size 8`.

This intentionally changes the meaning of existing explicit client arguments from bytes to MiB. Help text, documentation, and repository-owned scripts are updated together.

## Risks / Trade-offs

- [Operators expect decimal `MB`] -> Reject it clearly and document the accepted IEC suffixes.
- [A suffix typo silently changes meaning] -> Require complete, case-sensitive parsing; never ignore trailing characters.
- [Large magnitudes overflow during scaling] -> Check multiplication before calculating the final value.
- [Existing scripts pass raw client byte counts] -> Update repository-owned invocations and make the MiB unit explicit in help and documentation.
