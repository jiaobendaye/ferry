#include "concurrency_limiter.h"

namespace ferry
{

ConcurrencyLimiter::ConcurrencyLimiter(int cap)
	: cap_(cap)
{
}

bool ConcurrencyLimiter::try_acquire()
{
	if (this->disabled())
		return true;

	int cur = this->count_.fetch_add(1, std::memory_order_acq_rel);
	if (cur >= this->cap_)
	{
		this->count_.fetch_sub(1, std::memory_order_acq_rel);
		return false;
	}
	return true;
}

void ConcurrencyLimiter::release()
{
	if (this->disabled())
		return;
	this->count_.fetch_sub(1, std::memory_order_acq_rel);
}

bool ConcurrencyLimiter::try_acquire(const std::string& key)
{
	if (this->disabled())
		return true;

	std::lock_guard<std::mutex> lock(this->mutex_);
	int& cnt = this->keyed_[key];
	if (cnt >= this->cap_)
	{
		if (cnt == 0)
			this->keyed_.erase(key);	/* undo the default-insert */
		return false;
	}
	cnt++;
	return true;
}

void ConcurrencyLimiter::release(const std::string& key)
{
	if (this->disabled())
		return;

	std::lock_guard<std::mutex> lock(this->mutex_);
	auto it = this->keyed_.find(key);
	if (it == this->keyed_.end())
		return;							/* unbalanced release: ignore */
	if (--it->second <= 0)
		this->keyed_.erase(it);			/* zero -> no idle entries kept */
}

size_t ConcurrencyLimiter::size()
{
	std::lock_guard<std::mutex> lock(this->mutex_);
	return this->keyed_.size();
}

int ConcurrencyLimiter::current(const std::string& key)
{
	std::lock_guard<std::mutex> lock(this->mutex_);
	auto it = this->keyed_.find(key);
	return it == this->keyed_.end() ? 0 : it->second;
}

} // namespace ferry
