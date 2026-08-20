#include "token_bucket.h"

namespace ferry
{

TokenBucket::TokenBucket(long long rate, long long max_wait_sec, Clock clock)
	: rate_(rate),
	  max_wait_((long long)max_wait_sec * 1000),
	  clock_(std::move(clock)),
	  capacity_(rate > 0 ? (double)rate : 0.0),
	  tokens_(rate > 0 ? (double)rate : 0.0),	/* fresh bucket: 1s of burst */
	  last_refill_(this->clock_())
{
}

TokenBucket::Verdict TokenBucket::reserve(long long tokens)
{
	Verdict v;

	if (this->disabled() || tokens <= 0)
		return v;

	TimePoint now = this->clock_();
	std::lock_guard<std::mutex> lock(this->mutex_);

	/* lazy refill */
	double elapsed = std::chrono::duration<double>(now - this->last_refill_).count();
	if (elapsed > 0)
	{
		this->tokens_ += elapsed * (double)this->rate_;
		if (this->tokens_ > this->capacity_)
			this->tokens_ = this->capacity_;
		this->last_refill_ = now;
	}

	double deficit = (double)tokens - this->tokens_;
	if (deficit <= 0)
	{
		this->tokens_ -= (double)tokens;
		return v;						/* immediate */
	}

	long long wait_ms = (long long)(deficit * 1000.0 / this->rate_);
	if (wait_ms > this->max_wait_.count())
	{
		v.rejected = true;				/* no state change */
		return v;
	}

	this->tokens_ -= (double)tokens;	/* go negative; accrual pays it off */
	v.wait = Millis(wait_ms);
	return v;
}

} // namespace ferry
