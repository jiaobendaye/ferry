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
	return s;
}

std::string format_stats_line(const Stats::Snapshot& cur,
							  const Stats::Snapshot& prev,
							  long long qps_buckets, long long bw_buckets)
{
	char buf[512];

	snprintf(buf, sizeof(buf),
		"[stats] reqs=%lld(+%lld) 2xx=%lld 404=%lld 4xx=%lld 5xx=%lld "
		"rej(qps_total)=%lld rej(inflight)=%lld rej(qps_per_ip)=%lld "
		"rej(inflight_per_ip)=%lld rej(rate_total)=%lld rej(rate_per_ip)=%lld "
		"inflight=%d peak=%d buckets(qps)=%lld buckets(bw)=%lld served=%lld",
		cur.requests, cur.requests - prev.requests,
		cur.status_2xx, cur.status_404, cur.status_4xx_other, cur.status_5xx,
		cur.gate_rejects[Stats::GATE_QPS_TOTAL],
		cur.gate_rejects[Stats::GATE_INFLIGHT],
		cur.gate_rejects[Stats::GATE_QPS_PER_IP],
		cur.gate_rejects[Stats::GATE_INFLIGHT_PER_IP],
		cur.gate_rejects[Stats::GATE_RATE_TOTAL],
		cur.gate_rejects[Stats::GATE_RATE_PER_IP],
		cur.inflight, cur.inflight_peak,
		qps_buckets, bw_buckets,
		cur.bytes_served);
	return buf;
}

} // namespace ferry
