#ifndef FERRY_GATES_H
#define FERRY_GATES_H

#include <memory>

#include "concurrency_limiter.h"
#include "config/config.h"
#include "gate.h"
#include "rate_limiter.h"
#include "token_bucket.h"

namespace ferry
{

/*
 * Token-bucket admission gate (soft shaping, then 429).
 *
 * Exactly one of `bucket` (aggregate, no key) and `limiter` (per-IP,
 * keyed by *ctx.ip_key) is non-null. Units: charge_bytes == false ->
 * one unit per request (QPS); charge_bytes == true -> ctx.bytes units
 * (bandwidth; ctx.bytes <= 0 is a free pass, e.g. zero-length bodies).
 */
class TokenBucketGate : public Gate
{
public:
	TokenBucketGate(const char *name, Stats::GateId id,
					std::shared_ptr<TokenBucket> bucket,
					std::shared_ptr<RateLimiter> limiter,
					bool charge_bytes, int retry_after_sec);

	const char *name() const override { return this->name_; }
	Stats::GateId id() const override { return this->id_; }
	GateVerdict check(GateCtx& ctx) override;

private:
	const char *name_;
	Stats::GateId id_;
	std::shared_ptr<TokenBucket> bucket_;
	std::shared_ptr<RateLimiter> limiter_;
	bool charge_bytes_;
	int retry_after_sec_;
};

/*
 * Concurrency admission gate (hard reject, 503 + Retry-After: 1).
 * keyed == false -> global slot; keyed == true -> per-IP slot
 * (keyed by *ctx.ip_key). The release obligation is returned in the
 * verdict and MUST be kept until response completion.
 */
class SemaphoreGate : public Gate
{
public:
	SemaphoreGate(const char *name, Stats::GateId id,
				  std::shared_ptr<ConcurrencyLimiter> limiter, bool keyed);

	const char *name() const override { return this->name_; }
	Stats::GateId id() const override { return this->id_; }
	GateVerdict check(GateCtx& ctx) override;

private:
	const char *name_;
	Stats::GateId id_;
	std::shared_ptr<ConcurrencyLimiter> limiter_;
	bool keyed_;
};

/*
 * Everything build_gate_chains() constructs, including the per-IP
 * limiters, which main.cc also needs for the idle sweep and the stats
 * line. The limiters are owned by the gates' shared_ptrs as well, so
 * keeping this struct alive is sufficient but not required for gate
 * ownership — only for sweep/size access.
 */
struct GateSetup
{
	std::shared_ptr<GateChain> pre;			/* needs only ip_key */
	std::shared_ptr<GateChain> post;		/* needs response bytes */
	std::shared_ptr<RateLimiter> qps_per_ip_limiter;
	std::shared_ptr<RateLimiter> bw_per_ip_limiter;
	std::shared_ptr<ConcurrencyLimiter> inflight_limiter;
	std::shared_ptr<ConcurrencyLimiter> inflight_per_ip_limiter;
};

/*
 * Build both chains from configuration, omitting disabled gates.
 *
 *  pre-chain  (after client IP + ACL, before path resolution):
 *    qps_total -> max_inflight -> qps_per_ip -> max_inflight_per_ip
 *
 *  post-chain (after the Range decision knows the response length):
 *    rate_total_bps -> rate_bytes_per_sec
 *
 * Global gates precede per-IP gates so floods rejected by global caps
 * never create per-IP limiter state.
 */
GateSetup build_gate_chains(const ServerConfig& cfg);

} // namespace ferry

#endif // FERRY_GATES_H
