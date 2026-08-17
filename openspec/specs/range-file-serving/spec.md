# range-file-serving Specification

## Purpose

Serve files over plain HTTP from a configured root directory with full Range request semantics (200/206/413/416), per-response byte caps, reliable full-size discovery for download clients (HEAD, Content-Length, Content-Range), consistent response metadata, and path-safety guarantees against traversal.

## Requirements

### Requirement: Serve files from a configured root
The server SHALL serve regular files located under a configured root directory over HTTP (plain HTTP only), and SHALL respond `404 Not Found` when the requested path does not exist, is not a regular file, or falls outside the root.

#### Scenario: Existing file is served
- **WHEN** a client sends `GET /a/b.txt` and `<root>/a/b.txt` is a regular file within the size threshold and no Range header is present
- **THEN** the server responds `200 OK` with the complete file content as the body

#### Scenario: Missing file
- **WHEN** a client requests a path that does not exist under the root
- **THEN** the server responds `404 Not Found`

#### Scenario: Directory is not served
- **WHEN** a client requests a path that resolves to a directory
- **THEN** the server responds `404 Not Found`

### Requirement: Path safety
The server SHALL percent-decode the request URI path before mapping it to the filesystem and SHALL reject requests whose decoded path escapes the root directory (e.g. `../` sequences) or contains NUL bytes, responding `400 Bad Request`.

#### Scenario: Traversal attempt is rejected
- **WHEN** a client requests `GET /../../etc/passwd` or any percent-encoded equivalent that resolves outside the root
- **THEN** the server responds `400 Bad Request` and performs no file access outside the root

### Requirement: Response metadata on every response
Every response SHALL carry an `Accept-Ranges: bytes` header and an explicit `Content-Length` header. Responses that successfully stat a file SHALL carry `Last-Modified` set from the file's mtime.

#### Scenario: Headers present on a successful response
- **WHEN** the server responds with `200` or `206` for an existing file
- **THEN** the response contains `Accept-Ranges: bytes`, `Content-Length` equal to the number of body bytes sent, and `Last-Modified`

#### Scenario: Accept-Ranges advertised on errors
- **WHEN** the server responds with `404`, `413`, or `416`
- **THEN** the response still contains `Accept-Ranges: bytes`

### Requirement: Non-Range whole-file response bounded by threshold
For a request without a Range header, the server SHALL respond `200 OK` with the complete file when the file size is less than or equal to the configured size threshold, and SHALL respond `413 Content Too Large` with a self-describing body (instructing the client to use Range requests) when the file size exceeds the threshold.

#### Scenario: Small file served whole
- **WHEN** a client requests a file of size ≤ threshold without a Range header
- **THEN** the server responds `200` with `Content-Length` equal to the full file size and the complete body

#### Scenario: Large file rejected with 413
- **WHEN** a client requests a file of size > threshold without a Range header
- **THEN** the server responds `413` with a body that mentions the file size and instructs the client to use Range requests, and sends no file content

### Requirement: Satisfiable Range requests return 206
For a syntactically valid Range header with a satisfiable range, the server SHALL respond `206 Partial Content` with the requested byte interval, a `Content-Range: bytes <start>-<end>/<total>` header where `<total>` is the full file size, and `Content-Length` equal to the number of bytes in the body.

#### Scenario: Exact range
- **WHEN** a client requests `Range: bytes=0-99` for a 1000-byte file
- **THEN** the server responds `206` with bytes 0–99, `Content-Range: bytes 0-99/1000`, and `Content-Length: 100`

### Requirement: Responses are truncated to the configured cap
The server SHALL never return more than the configured cap bytes in a single response. When the requested (or implied) range is longer than the cap, the server SHALL return a shorter range of exactly `cap` bytes starting at the requested offset, keeping the full file size in the `Content-Range` total.

#### Scenario: Open-ended range is capped
- **WHEN** a client requests `Range: bytes=0-` for a file larger than the cap
- **THEN** the server responds `206` with the first `cap` bytes and `Content-Range: bytes 0-<cap-1>/<total>`

#### Scenario: Explicit wide range is capped
- **WHEN** a client requests a range whose length exceeds the cap
- **THEN** the server responds `206` with `cap` bytes starting at the requested offset, preserving the requested start in `Content-Range`

### Requirement: Suffix ranges
The server SHALL support suffix byte ranges (`bytes=-N`), returning the final `N` bytes of the file (or the entire file when `N` exceeds the file size), subject to the cap.

#### Scenario: Suffix range within file
- **WHEN** a client requests `Range: bytes=-100` for a 1000-byte file
- **THEN** the server responds `206` with bytes 900–999 and `Content-Range: bytes 900-999/1000`

#### Scenario: Suffix range larger than file
- **WHEN** a client requests `Range: bytes=-5000` for a 1000-byte file
- **THEN** the server responds `206` with the complete file content

### Requirement: Unsatisfiable ranges return 416
When the first byte position of a range is greater than or equal to the current file size, the server SHALL respond `416 Range Not Satisfiable` with a `Content-Range: bytes */<total>` header and no file content.

#### Scenario: Offset past end of file
- **WHEN** a client requests `Range: bytes=10000-` for a 1000-byte file
- **THEN** the server responds `416` with `Content-Range: bytes */1000`

#### Scenario: Resume of an already-complete download
- **WHEN** a client resumes from an offset equal to the file size
- **THEN** the server responds `416` with `Content-Range: bytes */<total>`, allowing the client to confirm completion

### Requirement: Multi-range requests serve the first range only
When a Range header contains multiple byte ranges, the server SHALL serve only the first range as a single-part `206` response and SHALL NOT produce `multipart/byteranges` bodies.

#### Scenario: Two ranges requested
- **WHEN** a client requests `Range: bytes=0-99,200-299`
- **THEN** the server responds `206` containing only bytes 0–99 with a single-part `Content-Range`

### Requirement: Invalid Range headers are ignored
When the Range header is syntactically invalid or uses an unsupported unit (anything other than `bytes`), the server SHALL ignore the header and process the request as if no Range header were present.

#### Scenario: Unsupported unit
- **WHEN** a client requests `Range: items=0-99`
- **THEN** the server treats the request as non-Range (200 whole file if ≤ threshold, otherwise 413)

### Requirement: HEAD requests return headers only and always reveal full size
For `HEAD` requests the server SHALL respond `200 OK` with `Content-Length` equal to the full file size for any existing regular file — regardless of the size threshold (the threshold only restricts GET body transfer) — and SHALL NOT send a body or read file content. HEAD of a missing path SHALL respond `404`. This makes HEAD a reliable size-probe mechanism for download clients.

#### Scenario: HEAD of a large file reveals its size
- **WHEN** a client sends `HEAD /large.bin` for a file larger than the threshold
- **THEN** the server responds `200` with `Content-Length` equal to the full file size, `Accept-Ranges: bytes`, and no body

#### Scenario: HEAD of a small file
- **WHEN** a client sends `HEAD` for a file within the threshold
- **THEN** the server responds `200` with `Content-Length` equal to the full file size and no body

#### Scenario: HEAD of a missing file
- **WHEN** a client sends `HEAD` for a nonexistent path
- **THEN** the server responds `404` with no body

### Requirement: If-Range validation against Last-Modified
The server SHALL support `If-Range` using the `Last-Modified` validator. When `If-Range` is present and does not match the file's current `Last-Modified`, the server SHALL process the request as if no Range header were present.

#### Scenario: Stale If-Range falls back to full representation semantics
- **WHEN** a client sends a Range request whose `If-Range` date is older than the file's mtime
- **THEN** the server ignores the Range header and applies the non-Range rules (200 whole or 413)

### Requirement: Configurable serving parameters
The cap (`cap_bytes`), size threshold (`size_threshold_bytes`), listen port, and root directory SHALL be configurable via the server configuration file. Missing keys SHALL fall back to documented defaults (cap = 8 MiB; threshold = cap). Invalid configuration values SHALL cause startup to fail with a clear error.

#### Scenario: Custom cap honored
- **WHEN** the server is configured with `cap_bytes = 1048576` and a client requests `Range: bytes=0-` for a 10 MiB file
- **THEN** the response contains exactly 1 MiB of data with `Content-Range` total of 10 MiB

#### Scenario: Invalid config fails startup
- **WHEN** the configuration file sets `cap_bytes = -1` or a non-numeric value
- **THEN** the server refuses to start and prints an error identifying the bad key
