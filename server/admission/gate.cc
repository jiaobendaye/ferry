#include "gate.h"

namespace ferry
{

void ReleaseList::append(ReleaseList&& other)
{
	for (auto& fn : other.fns_)
		this->fns_.push_back(std::move(fn));
	other.fns_.clear();				/* source gives up ownership */
}

void ReleaseList::run_all()
{
	for (auto it = this->fns_.rbegin(); it != this->fns_.rend(); ++it)
	{
		if (*it)
			(*it)();
	}
	this->fns_.clear();
}

ChainResult GateChain::run(GateCtx& ctx, Stats *stats)
{
	ChainResult r;
	ReleaseList acquired;			/* RAII rollback if we return early */

	for (auto& gate : this->gates_)
	{
		GateVerdict v = gate->check(ctx);

		if (v.rejected)
		{
			if (stats)
				stats->record_gate_reject(gate->id());
			r.rejected = true;
			r.status = v.status;
			r.retry_after_sec = v.retry_after_sec;
			return r;				/* acquired's dtor rolls back */
		}

		if (v.wait > r.delay)		/* compose delays: max, not sum */
			r.delay = v.wait;
		if (v.release)
			acquired.add(std::move(v.release));
	}

	r.releases = std::move(acquired);	/* hand off; no rollback */
	return r;
}

} // namespace ferry
