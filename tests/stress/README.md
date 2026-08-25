# ferry stress tests (opt-in, NOT part of CI)

Real-load tests that run for seconds to minutes. They are never executed
by the normal test targets — run them manually when you want numbers.

| Script | What it proves |
|---|---|
| `run_stress_qps.sh` | constant-rate load at 2× `qps_total` admits ~50% (the rest 429) |
| `run_stress_conc.sh` | 100 closed-loop workers against `max_inflight=20`: 503 overflow, stats peak exactly 20, drains to 0 |
| `run_stress_client.sh` | `ferry-client -j 32` completes a hash-verified download while one IP's gates push back most workers |
| `run_stress_soak.sh` | 10-minute shaping-backlog soak: stats lines continuous, inflight bounded, no transport errors |
| `run_stress_overhead.sh` | throughput gates-off vs gates-on — feeds the single-mutex-vs-sharding decision (design D12) |
| `run_stress_mmap.sh` | hot-cache A/B of pread vs mmap: MiB/s, CPU/GiB, latency, faults, RSS/RssAnon, and silent-fallback detection |
| `run_stress_cache.sh` | cold-file and repeated-reader comparison for `normal`, `noreuse`, and `drop_after_read`: integrity, residency, disk reads, CPU/GiB, memory classes, and advice counters |

## Usage

```bash
xmake                                   # release binaries required
tests/stress/run_stress_qps.sh
SOAK_SECONDS=60 tests/stress/run_stress_soak.sh   # short smoke soak
STRESS_SECONDS=20 QPS_TOTAL=500 tests/stress/run_stress_qps.sh
STRESS_SECONDS=10 CONCURRENCY=32 tests/stress/run_stress_mmap.sh
STRESS_SECONDS=3 CONCURRENCY=16 REPETITIONS=3 tests/stress/run_stress_mmap.sh
FILE_SIZE=524288000 CONCURRENCY=10 tests/stress/run_stress_cache.sh
```

Knobs are environment variables: `STRESS_SECONDS` (wall time),
`STRESS_PORT` (default 18990), plus per-script vars (`QPS_TOTAL`,
`MAX_INFLIGHT`, `CONCURRENCY`, `FILE_SIZE`, `SOAK_SECONDS`).

`run_stress_cache.sh` also accepts `CAP_BYTES` (default 8 MiB) and
`CACHE_WORK_ROOT` (default: the repository, which must be backed by a real
filesystem rather than tmpfs). `POLICIES=normal` selects a subset, while
`SERVER_BIN_OVERRIDE` permits benchmarking a separately built baseline binary.
`COLD_START_MAX_BYTES` sets the maximum accepted source residency before each
first read (default 4 MiB), preventing a falsely labelled warm run;
`IDLE_SECONDS` controls the post-download observation window (default 5).
Pass that run's JSON file as `BASELINE_RESULT` to enforce the normal-policy 3%
throughput and 5% CPU/GiB regression gates. By default the script performs two
complete, checksum-verified downloads per policy. The first read records cold
throughput and disk I/O; the second makes the repeated-reader cost explicit. It
fails if drop cold throughput regresses more than 10%, `drop_after_read` leaves
more than `max(64 MiB, 2 × CAP_BYTES)` of the target file resident after the
idle interval, or cache advice reports errors.

The load driver is `ferry_stress.py` (self-contained python3, open-loop
`rate` and closed-loop `closed` modes, JSON results). If you prefer
external tools, `hey`/`vegeta`/`wrk` can replace the driver invocations
one-for-one — e.g. `vegeta attack -rate $((2*QPS)) -duration ${DUR}s`.

These scripts do NOT run under ASan/TSan builds (the numbers would be
meaningless); use a plain release build.

## Gates overhead baseline (D12)

Measured on 2026-08-18 in the local development container with:

```bash
STRESS_SECONDS=5 CONCURRENCY=32 tests/stress/run_stress_overhead.sh
```

| Configuration | Throughput |
|---|---:|
| all admission gates off | 5260.5 req/s |
| all admission gates on (non-binding limits) | 5250.5 req/s |

Observed difference: **0.2%**. This baseline does not justify sharding the
keyed limiter maps; keep the simpler single-mutex implementation for now.
Absolute throughput is host-dependent, so rerun the script on production-like
hardware before using the number for capacity planning.
