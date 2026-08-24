# resumable-download-client Specification

## Purpose

Multi-threaded, resumable HTTP download client: fixed-size chunks dynamically claimed by parallel workers, resume state persisted atomically (.part + meta JSON), generic Range-server probing with single-stream fallback, chunk-level retry with exponential backoff and Retry-After handling, and a streaming sha256 gate that controls when the output file appears.
## Requirements
### Requirement: Chunk planning and dynamic claiming
The client SHALL divide the file into fixed-size chunks of `chunk_size` bytes (default 8 MiB, configurable) and SHALL claim chunks dynamically: each worker atomically takes the next unclaimed-and-incomplete chunk until none remain. The number of active workers SHALL be `min(jobs, chunk_count)`.

#### Scenario: Worker claims until exhausted
- **WHEN** a 20 MiB file is downloaded with chunk_size 8 MiB and jobs 4
- **THEN** 3 chunks exist, 3 workers run, and every chunk is downloaded exactly once

#### Scenario: Single chunk file
- **WHEN** the file is smaller than chunk_size
- **THEN** exactly one chunk exists and one worker downloads it

### Requirement: Range mode memory bound
In Range mode every request SHALL ask for at most `chunk_size` bytes, and the client SHALL verify each 206 response: body length equals the Content-Range interval length, interval start equals the requested offset, interval end does not exceed the requested logical chunk, and total equals the expected file size. A response violating these SHALL be treated as a failed chunk attempt. When a valid response ends before the logical chunk boundary, the client SHALL write the received interval and request the remaining suffix; it SHALL mark the logical chunk complete only after every byte in that chunk has been written.

#### Scenario: Oversized or misaligned response rejected
- **WHEN** a 206 response's body length differs from its Content-Range interval length or its interval falls outside the requested logical chunk
- **THEN** the chunk attempt fails and enters the retry path without writing data

#### Scenario: Total mismatch is fatal
- **WHEN** a 206 response's Content-Range total differs from the expected file size
- **THEN** the download fails with a clear error

#### Scenario: Short response continues the logical chunk
- **WHEN** a client requests a 2 MiB logical chunk and the server returns valid consecutive responses capped at 1 MiB
- **THEN** the client requests the second 1 MiB suffix and marks the logical chunk complete only after both intervals are written

### Requirement: Server capability probing
The client SHALL probe before downloading: HEAD first; when HEAD lacks Content-Length or Accept-Ranges information (or fails), a `GET` with `Range: bytes=0-0` decides the mode — `206` selects chunk mode, `200` selects single-stream mode. A probe answered with `200` and a full body SHALL be treated as the beginning of the download, not discarded.

#### Scenario: Full information from HEAD
- **WHEN** HEAD returns Content-Length and `Accept-Ranges: bytes`
- **THEN** the client enters chunk mode without a probe GET

#### Scenario: Non-Range server probe becomes the download
- **WHEN** the probe GET receives 200 with the complete body
- **THEN** the client enters single-stream mode and writes that body as the start of the file

### Requirement: Single-stream fallback with size limit
For servers without Range support the client SHALL download in a single stream and SHALL enforce `--single-stream-limit` (default 256 MiB): when the size is known and exceeds the limit the client SHALL refuse before downloading; when the size is unknown it SHALL abort as soon as received bytes exceed the limit.

#### Scenario: Known oversize refused up front
- **WHEN** the server ignores Range and HEAD reported a size above the limit
- **THEN** the client exits with an error and downloads nothing

#### Scenario: Unknown size aborted mid-stream
- **WHEN** received bytes exceed the limit during an unknown-size single-stream download
- **THEN** the client aborts with a clear error

### Requirement: Chunked responses decoded
When a response uses chunked transfer encoding, the client SHALL decode it before validation and writing.

#### Scenario: Chunked 200 handled
- **WHEN** a single-stream response is chunked-encoded
- **THEN** the decoded content is written and counted correctly

### Requirement: Resume persistence
The client SHALL write data to `<output>.part` and state to `<output>.ferry.json` containing the url, file size, Last-Modified, chunk_size, and completion bitmap. A chunk SHALL be marked complete only after its data is successfully written. The meta file SHALL be replaced atomically (temp write + rename).

#### Scenario: Marking order guarantees safety
- **WHEN** the process is killed immediately after a chunk's write completes but before the meta rewrite
- **THEN** a later resume re-downloads at most that one chunk and never produces a corrupt file

### Requirement: Resume consistency check
On restart with existing state, the client SHALL HEAD the URL and compare size and Last-Modified with the meta file. On a match it SHALL resume using the bitmap; on a mismatch or unreadable meta it SHALL warn, discard the state, and restart from scratch.

#### Scenario: File changed on server
- **WHEN** the server's Last-Modified differs from the stored value
- **THEN** the client warns, deletes `.part` and meta, and restarts the download

#### Scenario: Resume skips completed chunks
- **WHEN** resuming with a bitmap showing chunks 0–2 complete
- **THEN** only the remaining chunks are requested

### Requirement: Graceful stop on SIGINT
On SIGINT the client SHALL stop claiming new chunks, persist the current bitmap, exit with a non-zero code, and leave `.part` and meta in place with a message that the same command resumes.

#### Scenario: SIGINT leaves resumable state
- **WHEN** SIGINT arrives mid-download
- **THEN** the process exits non-zero and re-running the same command resumes from the saved bitmap

### Requirement: Chunk retry with exponential backoff
For network errors, timeouts, and 5xx responses the client SHALL retry the chunk with delay `min(500ms × 2^attempts, 30s)`, up to 8 attempts, after which the whole download SHALL fail with `.part` and meta kept. Delays SHALL not block threads.

#### Scenario: Backoff sequence
- **WHEN** a chunk fails three consecutive times
- **THEN** the waits before retries 1–3 are approximately 0.5 s, 1 s, and 2 s

#### Scenario: Attempts exhausted
- **WHEN** a chunk fails 8 consecutive times
- **THEN** the download fails, state files are kept, and the exit code is non-zero

### Requirement: Rate-limit responses honored
On `429` the client SHALL wait `max(Retry-After, the backoff value)` and retry the chunk, counting it as one attempt.

#### Scenario: Retry-After floors the wait
- **WHEN** the server answers 429 with `Retry-After: 5` and the backoff value is 1 s
- **THEN** the client waits at least 5 s before retrying

### Requirement: Fatal status propagation
On `403` or `404` the client SHALL stop all workers promptly, exit non-zero with the status identified, and keep `.part` and meta. `416` for an offset equal to the file size SHALL be treated as completion of that chunk.

#### Scenario: 403 stops the fleet
- **WHEN** any worker receives 403
- **THEN** no further chunks are claimed and the process exits non-zero

### Requirement: Receive timeout larger than server shaping
The per-request receive timeout SHALL default to 60 seconds and be configurable, so that a rate-limiting ferry server's soft-shaping delays (bounded by its `max_wait_sec`) are not mistaken for failures.

#### Scenario: Shaped response not treated as timeout
- **WHEN** the server delays a capped response by less than its max wait
- **THEN** the client receives it successfully without a timeout retry

### Requirement: Final verification gates the output file
After all chunks complete the client SHALL compute sha256 of the assembled file by streaming reads unless `--no-verify` is given, and SHALL print the digest. With `--checksum sha-256=<hex>` the digest MUST match for the download to succeed; on mismatch the client SHALL keep the files and exit non-zero. Only successful verification (or `--no-verify`) SHALL rename `.part` to the output file and remove the meta file.

#### Scenario: Checksum mismatch keeps evidence
- **WHEN** the computed digest differs from `--checksum`
- **THEN** `.part` and meta remain, no output file is created, and the exit code is non-zero

#### Scenario: Success renames
- **WHEN** verification passes (or is skipped)
- **THEN** the output file exists at the requested path and the meta file is removed

### Requirement: Numeric progress reporting
Unless `--quiet`, the client SHALL print one progress line per second to stderr containing percent, downloaded/total bytes, current speed, ETA, chunks done/total, and retry count, followed by a final summary with elapsed time, average speed, and the sha256 digest.

#### Scenario: Progress line contents
- **WHEN** a download is running without `--quiet`
- **THEN** stderr receives periodic lines containing percentage, byte counts, speed, ETA, chunk progress, and retries

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
