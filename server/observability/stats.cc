#include <cstdio>
#include "stats.h"

namespace ferry
{

void Stats::record_request(const char *status, long long bytes_served)
{
	this->requests_.fetch_add(1, std::memory_order_relaxed);

	switch (status ? status[0] : '\0')
	{
	case '2':
		this->status_2xx_.fetch_add(1, std::memory_order_relaxed);
		if (bytes_served > 0)
			this->bytes_served_.fetch_add(bytes_served,
										  std::memory_order_relaxed);
		break;
	case '4':
		if (status[1] == '0' && status[2] == '4' && status[3] == '\0')
			this->status_404_.fetch_add(1, std::memory_order_relaxed);
		else
			this->status_4xx_other_.fetch_add(1, std::memory_order_relaxed);
		break;
	case '5':
		this->status_5xx_.fetch_add(1, std::memory_order_relaxed);
		break;
	default:
		this->status_4xx_other_.fetch_add(1, std::memory_order_relaxed);
		break;
	}
}

void Stats::record_gate_reject(GateId id)
{
	if (id >= 0 && id < GATE_COUNT)
		this->gate_rejects_[id].fetch_add(1, std::memory_order_relaxed);
}

void Stats::inflight_inc()
{
	int v = this->inflight_.fetch_add(1, std::memory_order_acq_rel) + 1;
	int peak = this->inflight_peak_.load(std::memory_order_relaxed);

	while (v > peak &&
		   !this->inflight_peak_.compare_exchange_weak(
					peak, v, std::memory_order_relaxed))
	{
	}
}

void Stats::inflight_dec()
{
	this->inflight_.fetch_sub(1, std::memory_order_acq_rel);
}

void Stats::mmap_open(long long bytes)
{
	this->mmap_responses_.fetch_add(1, std::memory_order_relaxed);
	this->mmap_bytes_.fetch_add(bytes, std::memory_order_relaxed);
	long long active = this->mmap_active_bytes_.fetch_add(
						bytes, std::memory_order_acq_rel) + bytes;
	long long peak = this->mmap_active_peak_.load(std::memory_order_relaxed);

	while (active > peak &&
		   !this->mmap_active_peak_.compare_exchange_weak(
					peak, active, std::memory_order_relaxed))
	{
	}
}

void Stats::mmap_close(long long bytes)
{
	this->mmap_active_bytes_.fetch_sub(bytes, std::memory_order_acq_rel);
}

void Stats::mmap_fallback()
{
	this->mmap_fallbacks_.fetch_add(1, std::memory_order_relaxed);
}

void Stats::record_cache_advice(long long accepted_bytes, bool error)
{
	this->cache_advice_calls_.fetch_add(1, std::memory_order_relaxed);
	if (accepted_bytes > 0)
		this->cache_advice_bytes_.fetch_add(accepted_bytes,
											 std::memory_order_relaxed);
	if (error)
		this->cache_advice_errors_.fetch_add(1, std::memory_order_relaxed);
}

Stats::Snapshot Stats::snapshot() const
{
	Snapshot s;

	s.requests = this->requests_.load(std::memory_order_relaxed);
	s.status_2xx = this->status_2xx_.load(std::memory_order_relaxed);
	s.status_404 = this->status_404_.load(std::memory_order_relaxed);
	s.status_4xx_other = this->status_4xx_other_.load(std::memory_order_relaxed);
	s.status_5xx = this->status_5xx_.load(std::memory_order_relaxed);
	for (int i = 0; i < GATE_COUNT; i++)
		s.gate_rejects[i] = this->gate_rejects_[i].load(std::memory_order_relaxed);
	s.bytes_served = this->bytes_served_.load(std::memory_order_relaxed);
	s.inflight = this->inflight_.load(std::memory_order_relaxed);
	s.inflight_peak = this->inflight_peak_.load(std::memory_order_relaxed);
	s.mmap_responses = this->mmap_responses_.load(std::memory_order_relaxed);
	s.mmap_bytes = this->mmap_bytes_.load(std::memory_order_relaxed);
	s.mmap_active_bytes = this->mmap_active_bytes_.load(
											std::memory_order_relaxed);
	s.mmap_active_peak = this->mmap_active_peak_.load(std::memory_order_relaxed);
	s.mmap_fallbacks = this->mmap_fallbacks_.load(std::memory_order_relaxed);
	s.cache_advice_calls = this->cache_advice_calls_.load(
												std::memory_order_relaxed);
	s.cache_advice_bytes = this->cache_advice_bytes_.load(
												std::memory_order_relaxed);
	s.cache_advice_errors = this->cache_advice_errors_.load(
												 std::memory_order_relaxed);
	return s;
}

std::string format_stats_line(const Stats::Snapshot& cur,
							  const Stats::Snapshot& prev,
							  long long qps_buckets, long long bw_buckets,
							  CgroupMemorySnapshot memory)
{
	char buf[1024];

	snprintf(buf, sizeof(buf),
		"[stats] reqs=%lld(+%lld) 2xx=%lld 404=%lld 4xx=%lld 5xx=%lld "
		"rej(qps_total)=%lld rej(inflight)=%lld rej(qps_per_ip)=%lld "
		"rej(inflight_per_ip)=%lld rej(rate_total)=%lld rej(rate_per_ip)=%lld "
		"inflight=%d peak=%d buckets(qps)=%lld buckets(bw)=%lld served=%lld "
		"mmap_resps=%lld mmap_bytes=%lld mmap_active=%lld mmap_peak=%lld "
		"mmap_fallbacks=%lld cache_advice_calls=%lld "
		"cache_advice_bytes=%lld cache_advice_errors=%lld "
		"mem_anon=%lld mem_file=%lld mem_sock=%lld",
		cur.requests, cur.requests - prev.requests,
		cur.status_2xx, cur.status_404, cur.status_4xx_other, cur.status_5xx,
		cur.gate_rejects[Stats::GATE_QPS_TOTAL],
		cur.gate_rejects[Stats::GATE_INFLIGHT],
		cur.gate_rejects[Stats::GATE_QPS_PER_IP],
		cur.gate_rejects[Stats::GATE_INFLIGHT_PER_IP],
		cur.gate_rejects[Stats::GATE_RATE_TOTAL],
		cur.gate_rejects[Stats::GATE_RATE_PER_IP],
		cur.inflight, cur.inflight_peak,
		qps_buckets, bw_buckets, cur.bytes_served,
		cur.mmap_responses, cur.mmap_bytes, cur.mmap_active_bytes,
		cur.mmap_active_peak, cur.mmap_fallbacks,
		cur.cache_advice_calls, cur.cache_advice_bytes, cur.cache_advice_errors,
		memory.anon, memory.file, memory.sock);
	return buf;
}

} // namespace ferry
