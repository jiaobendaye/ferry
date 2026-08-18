#include <tuple>
#include "rate_limiter.h"

namespace ferry
{

RateLimiter::RateLimiter(long long rate_bps, long long max_wait_sec, Clock clock)
	: rate_bps_(rate_bps),
	  max_wait_sec_(max_wait_sec),
	  clock_(std::move(clock))
{
}

RateLimiter::Verdict RateLimiter::reserve(const std::string& ip_key,
										  long long tokens)
{
	Verdict v;

	if (this->disabled() || tokens <= 0)
		return v;

	TimePoint now = this->clock_();
	std::lock_guard<std::mutex> lock(this->mutex_);

	auto it = this->buckets_.find(ip_key);
	if (it == this->buckets_.end())
	{
		it = this->buckets_.emplace(
				std::piecewise_construct,
				std::forward_as_tuple(ip_key),
				std::forward_as_tuple(this->rate_bps_, this->max_wait_sec_,
									  this->clock_, now)).first;
	}

	Entry& e = it->second;
	e.last_active = now;
	return e.bucket.reserve(tokens);
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
