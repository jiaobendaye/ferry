#ifndef FERRY_STATS_H
#define FERRY_STATS_H

#include <array>
#include <atomic>
#include <string>

#include "cgroup_memory.h"

namespace ferry
{

/*
 * Runtime counters for observability. All updates are relaxed atomic
 * increments on the request hot path; snapshot() takes a consistent-enough
 * plain copy for formatting. Reconciliation invariant: requests equals the
 * sum of the status counters (every request is recorded exactly once, at
 * response completion, on both sync and async paths).
 */
class Stats
{
public:
	/* Fixed gate identities; rejection counters are indexed by these so
	   adding a gate never touches the Stats layout. Keep in sync with
	   the gate names built by build_gate_chains(). */
	enum GateId
	{
		GATE_QPS_TOTAL = 0,
		GATE_INFLIGHT,
		GATE_QPS_PER_IP,
		GATE_INFLIGHT_PER_IP,
		GATE_RATE_TOTAL,
		GATE_RATE_PER_IP,
		GATE_COUNT
	};

	/* Record one completed request. `status` is the final HTTP status
	   code string; `bytes_served` is the file-content bytes actually
	   sent (0 for HEAD and all error replies). */
	void record_request(const char *status, long long bytes_served);

	void record_gate_reject(GateId id);

	/* In-flight gauge: inc at admission (after all gates pass),
	   dec at response completion. */
	void inflight_inc();
	void inflight_dec();

	/* Experimental mmap body-path observability. `mmap_open` is called
	   only after a mapping succeeds; fallback records failed mmap attempts. */
	void mmap_open(long long bytes);
	void mmap_close(long long bytes);
	void mmap_fallback();

	/* Best-effort file-cache advice accounting. `accepted_bytes` is the
	   range accepted by the syscall, not proof of physical eviction. */
	void record_cache_advice(long long accepted_bytes, bool error);

	struct Snapshot
	{
		long long requests = 0;
		long long status_2xx = 0;
		long long status_404 = 0;
		long long status_4xx_other = 0;
		long long status_5xx = 0;
		long long gate_rejects[GATE_COUNT] = {0};
		long long bytes_served = 0;
		int inflight = 0;
		int inflight_peak = 0;
		long long mmap_responses = 0;
		long long mmap_bytes = 0;
		long long mmap_active_bytes = 0;
		long long mmap_active_peak = 0;
		long long mmap_fallbacks = 0;
		long long cache_advice_calls = 0;
		long long cache_advice_bytes = 0;
		long long cache_advice_errors = 0;
	};

	Snapshot snapshot() const;

private:
	std::atomic<long long> requests_{0};
	std::atomic<long long> status_2xx_{0};
	std::atomic<long long> status_404_{0};
	std::atomic<long long> status_4xx_other_{0};
	std::atomic<long long> status_5xx_{0};
	std::array<std::atomic<long long>, GATE_COUNT> gate_rejects_{};
	std::atomic<long long> bytes_served_{0};
	std::atomic<int> inflight_{0};
	std::atomic<int> inflight_peak_{0};
	std::atomic<long long> mmap_responses_{0};
	std::atomic<long long> mmap_bytes_{0};
	std::atomic<long long> mmap_active_bytes_{0};
	std::atomic<long long> mmap_active_peak_{0};
	std::atomic<long long> mmap_fallbacks_{0};
	std::atomic<long long> cache_advice_calls_{0};
	std::atomic<long long> cache_advice_bytes_{0};
	std::atomic<long long> cache_advice_errors_{0};
};

/*
 * One-line key=value summary with per-interval deltas (cur - prev),
 * suitable for stderr logging and grep/awk. `qps_buckets`/`bw_buckets`
 * are the active per-IP map sizes of the corresponding limiters
 * (0 when the limiter is disabled).
 */
std::string format_stats_line(const Stats::Snapshot& cur,
							  const Stats::Snapshot& prev,
							  long long qps_buckets, long long bw_buckets,
							  CgroupMemorySnapshot memory = CgroupMemorySnapshot());

} // namespace ferry

#endif // FERRY_STATS_H
