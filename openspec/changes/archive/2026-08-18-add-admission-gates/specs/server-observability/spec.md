## ADDED Requirements

### Requirement: Request and status accounting
The server SHALL count every request and classify each by final response status: successful file responses (200/206), 404, other 4xx, and 5xx. Counts SHALL be recorded at a single completion point so that synchronous error replies and asynchronous file responses are accounted identically.

#### Scenario: Counts reconcile
- **WHEN** any mix of requests completes (successes, 404s, 429s, 503s)
- **THEN** the total request counter equals the sum of the per-status counters

#### Scenario: Async and sync paths both counted
- **WHEN** a burst contains both fast error replies and slow file responses
- **THEN** every request appears in exactly one status counter after completion

### Requirement: Per-gate rejection accounting
Each admission gate SHALL record its own rejection count, keyed by gate name, so operators can distinguish which limit engaged (e.g. global QPS vs per-IP bandwidth vs concurrency).

#### Scenario: Rejections attributed to the rejecting gate
- **WHEN** requests are rejected by the global concurrency gate and by the per-IP QPS gate in the same interval
- **THEN** the stats report shows separate nonzero counts for each gate and the counts sum to the total 429+503 replies

### Requirement: In-flight gauge and peak
The server SHALL expose the current number of in-flight requests and the peak observed since startup. The gauge SHALL read zero whenever no request is being processed.

#### Scenario: Gauge tracks load and drains
- **WHEN** concurrent requests are in flight and then all complete
- **THEN** the gauge was positive during the burst, the peak is at least the burst size, and the gauge returns to zero

### Requirement: Served bytes accounting
The server SHALL count the total number of file-content bytes served (200/206 bodies).

#### Scenario: Bytes counted for file responses only
- **WHEN** a 206 response of N bytes and a 404 response complete
- **THEN** the served-bytes counter increases by N

### Requirement: Periodic stats line
When `stats_interval_sec` is set to a positive value, the server SHALL emit one single-line `key=value` stats summary to stderr every interval, containing at least: total requests with the per-interval delta, per-status counts, per-gate rejection counts, current/peak in-flight, and served bytes. A value of zero or unset SHALL disable periodic output.

#### Scenario: Periodic line emitted
- **WHEN** `stats_interval_sec` is 5 and the server handles traffic for 12 seconds
- **THEN** at least two stats lines appear on stderr, each parseable as single-line key=value pairs, with the request delta reflecting the interval's traffic

#### Scenario: Disabled by default
- **WHEN** `stats_interval_sec` is unset
- **THEN** no periodic stats output is produced

### Requirement: On-demand stats dump via SIGUSR1
The server SHALL dump a full stats summary promptly (within one second) upon receiving SIGUSR1, independent of the periodic interval. The signal handler SHALL only set a flag; formatting and printing SHALL happen outside signal context.

#### Scenario: Dump on demand
- **WHEN** the server receives SIGUSR1 with periodic stats disabled
- **THEN** a full stats summary is printed within one second

#### Scenario: Repeated dumps
- **WHEN** the server receives SIGUSR1 twice at different times
- **THEN** two summaries are printed and the second reflects counters at least as large as the first

### Requirement: Stats configuration
The key `stats_interval_sec` SHALL be configurable, defaulting to off, and invalid values SHALL cause startup to fail with a clear error.

#### Scenario: Invalid interval fails startup
- **WHEN** the configuration sets `stats_interval_sec` to a negative number
- **THEN** the server refuses to start with an error naming the key
