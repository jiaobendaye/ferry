#include <utility>

#include "gates.h"

namespace ferry
{

/* ---------------- TokenBucketGate ---------------- */

TokenBucketGate::TokenBucketGate(const char *name, Stats::GateId id,
								 std::shared_ptr<TokenBucket> bucket,
								 std::shared_ptr<RateLimiter> limiter,
								 bool charge_bytes, int retry_after_sec)
	: name_(name),
	  id_(id),
	  bucket_(std::move(bucket)),
	  limiter_(std::move(limiter)),
	  charge_bytes_(charge_bytes),
	  retry_after_sec_(retry_after_sec)
{
}

GateVerdict TokenBucketGate::check(GateCtx& ctx)
{
	GateVerdict v;
	long long units = this->charge_bytes_ ? ctx.bytes : 1;

	TokenBucket::Verdict tv = this->limiter_ ?
		this->limiter_->reserve(*ctx.ip_key, units) :
		this->bucket_->reserve(units);

	if (tv.rejected)
	{
		v.rejected = true;
		v.status = "429";
		v.retry_after_sec = this->retry_after_sec_;
	}
	else
		v.wait = tv.wait;

	return v;
}

/* ---------------- SemaphoreGate ---------------- */

SemaphoreGate::SemaphoreGate(const char *name, Stats::GateId id,
							 std::shared_ptr<ConcurrencyLimiter> limiter,
							 bool keyed)
	: name_(name),
	  id_(id),
	  limiter_(std::move(limiter)),
	  keyed_(keyed)
{
}

GateVerdict SemaphoreGate::check(GateCtx& ctx)
{
	GateVerdict v;
	bool ok;

	if (this->keyed_)
		ok = this->limiter_->try_acquire(*ctx.ip_key);
	else
		ok = this->limiter_->try_acquire();

	if (!ok)
	{
		v.rejected = true;
		v.status = "503";
		v.retry_after_sec = 1;
		return v;
	}

	auto lim = this->limiter_;

	if (this->keyed_)
	{
		std::string key = *ctx.ip_key;
		v.release = [lim, key]() { lim->release(key); };
	}
	else
		v.release = [lim]() { lim->release(); };

	return v;
}

/* ---------------- chain factory ---------------- */

GateSetup build_gate_chains(const ServerConfig& cfg)
{
	GateSetup s;

	s.pre = std::make_shared<GateChain>();
	s.post = std::make_shared<GateChain>();

	if (cfg.qps_total > 0)
	{
		s.pre->add(std::make_unique<TokenBucketGate>(
			"qps_total", Stats::GATE_QPS_TOTAL,
			std::make_shared<TokenBucket>(cfg.qps_total, cfg.max_wait_sec),
			nullptr, false, cfg.max_wait_sec));
	}

	if (cfg.max_inflight > 0)
	{
		s.inflight_limiter =
			std::make_shared<ConcurrencyLimiter>(cfg.max_inflight);
		s.pre->add(std::make_unique<SemaphoreGate>(
			"max_inflight", Stats::GATE_INFLIGHT,
			s.inflight_limiter, false));
	}

	if (cfg.qps_per_ip > 0)
	{
		s.qps_per_ip_limiter =
			std::make_shared<RateLimiter>(cfg.qps_per_ip, cfg.max_wait_sec);
		s.pre->add(std::make_unique<TokenBucketGate>(
			"qps_per_ip", Stats::GATE_QPS_PER_IP,
			nullptr, s.qps_per_ip_limiter, false, cfg.max_wait_sec));
	}

	if (cfg.max_inflight_per_ip > 0)
	{
		s.inflight_per_ip_limiter =
			std::make_shared<ConcurrencyLimiter>(cfg.max_inflight_per_ip);
		s.pre->add(std::make_unique<SemaphoreGate>(
			"max_inflight_per_ip", Stats::GATE_INFLIGHT_PER_IP,
			s.inflight_per_ip_limiter, true));
	}

	if (cfg.rate_total_bps > 0)
	{
		s.post->add(std::make_unique<TokenBucketGate>(
			"rate_total", Stats::GATE_RATE_TOTAL,
			std::make_shared<TokenBucket>(cfg.rate_total_bps,
										  cfg.max_wait_sec),
			nullptr, true, cfg.max_wait_sec));
	}

	if (cfg.rate_bytes_per_sec > 0)
	{
		s.bw_per_ip_limiter =
			std::make_shared<RateLimiter>(cfg.rate_bytes_per_sec,
										  cfg.max_wait_sec);
		s.post->add(std::make_unique<TokenBucketGate>(
			"rate_per_ip", Stats::GATE_RATE_PER_IP,
			nullptr, s.bw_per_ip_limiter, true, cfg.max_wait_sec));
	}

	return s;
}

} // namespace ferry
