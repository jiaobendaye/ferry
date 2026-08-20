#ifndef FERRY_HANDLER_H
#define FERRY_HANDLER_H

#include <memory>
#include <string>
#include "workflow/WFHttpServer.h"
#include "access/acl.h"
#include "access/client_ip.h"
#include "admission/gate.h"
#include "config/config.h"
#include "observability/stats.h"

namespace ferry
{

/*
 * The request pipeline:
 *   client IP (XFF/peer) -> ACL
 *   -> pre-chain gates (QPS/concurrency: global then per-IP)
 *   -> path safety -> stat -> HEAD shortcut -> If-Range -> Range
 *      decision (cap/threshold)
 *   -> post-chain gates (bandwidth: global then per-IP)
 *   -> timer (composed shaping delay) -> configured file body path:
 *      async pread (default) or mmap-backed nocopy body (experimental)
 *
 * Every request gets a unified completion callback (all paths, sync and
 * async) that releases gate resources, frees/unmaps file bodies, and records
 * the final status into Stats.
 */
class Handler
{
public:
	Handler(const ServerConfig& cfg, std::shared_ptr<Acl> acl,
			std::shared_ptr<GateChain> pre_chain,
			std::shared_ptr<GateChain> post_chain,
			std::shared_ptr<Stats> stats);

	void process(WFHttpTask *server_task);

	const ServerConfig& config() const { return this->cfg_; }
	std::shared_ptr<Stats> stats() const { return this->stats_; }

private:
	ServerConfig cfg_;						/* root normalized (no trailing '/') */
	std::shared_ptr<Acl> acl_;				/* may be null: ACL disabled */
	std::shared_ptr<GateChain> pre_chain_;	/* never null; may be empty */
	std::shared_ptr<GateChain> post_chain_;	/* never null; may be empty */
	std::shared_ptr<Stats> stats_;
};

} // namespace ferry

#endif // FERRY_HANDLER_H
