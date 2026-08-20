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

## Usage

```bash
xmake                                   # release binaries required
tests/stress/run_stress_qps.sh
SOAK_SECONDS=60 tests/stress/run_stress_soak.sh   # short smoke soak
STRESS_SECONDS=20 QPS_TOTAL=500 tests/stress/run_stress_qps.sh
STRESS_SECONDS=10 CONCURRENCY=32 tests/stress/run_stress_mmap.sh
STRESS_SECONDS=3 CONCURRENCY=16 REPETITIONS=3 tests/stress/run_stress_mmap.sh
```

Knobs are environment variables: `STRESS_SECONDS` (wall time),
`STRESS_PORT` (default 18990), plus per-script vars (`QPS_TOTAL`,
`MAX_INFLIGHT`, `CONCURRENCY`, `FILE_SIZE`, `SOAK_SECONDS`).

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
