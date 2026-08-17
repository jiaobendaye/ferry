#include "rate_limiter.h"

namespace ferry
{

RateLimiter::RateLimiter(long long rate_bps, long long max_wait_sec, Clock clock)
	: rate_bps_(rate_bps),
	  max_wait_((long long)max_wait_sec * 1000),
	  clock_(std::move(clock)),
	  capacity_(rate_bps > 0 ? (double)rate_bps : 0.0)
{
}

RateLimiter::Verdict RateLimiter::reserve(const std::string& ip_key,
										  long long bytes)
{
	Verdict v;

	if (this->disabled() || bytes <= 0)
		return v;

	TimePoint now = this->clock_();
	std::lock_guard<std::mutex> lock(this->mutex_);

	auto it = this->buckets_.find(ip_key);
	if (it == this->buckets_.end())
	{
		Bucket b;
		b.tokens = this->capacity_;		/* fresh clients get 1s of burst */
		b.last_refill = now;
		b.last_active = now;
		it = this->buckets_.emplace(ip_key, b).first;
	}

	Bucket& b = it->second;

	/* lazy refill */
	double elapsed = std::chrono::duration<double>(now - b.last_refill).count();
	if (elapsed > 0)
	{
		b.tokens += elapsed * (double)this->rate_bps_;
		if (b.tokens > this->capacity_)
			b.tokens = this->capacity_;
		b.last_refill = now;
	}
	b.last_active = now;

	double deficit = (double)bytes - b.tokens;
	if (deficit <= 0)
	{
		b.tokens -= (double)bytes;
		return v;						/* immediate */
	}

	long long wait_ms = (long long)(deficit * 1000.0 / this->rate_bps_);
	if (wait_ms > this->max_wait_.count())
	{
		v.rejected = true;				/* no state change */
		return v;
	}

	b.tokens -= (double)bytes;			/* go negative; accrual pays it off */
	v.wait = Millis(wait_ms);
	return v;
}

void RateLimiter::sweep(Millis idle_threshold)
{
	TimePoint now = this->clock_();
	std::lock_guard<std::mutex> lock(this->mutex_);

	for (auto it = this->buckets_.begin(); it != this->buckets_.end();)
	{
		Millis idle = std::chrono::duration_cast<Millis>(now -
												it->second.last_active);
		if (idle >= idle_threshold)
			it = this->buckets_.erase(it);
		else
			++it;
	}
}

size_t RateLimiter::size()
{
	std::lock_guard<std::mutex> lock(this->mutex_);
	return this->buckets_.size();
}

} // namespace ferry
