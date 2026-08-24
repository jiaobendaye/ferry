## MODIFIED Requirements

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
