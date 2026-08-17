#ifndef WF_HANDLER_H
#define WF_HANDLER_H

#include <memory>
#include <string>
#include "workflow/WFHttpServer.h"
#include "acl.h"
#include "client_ip.h"
#include "config.h"
#include "rate_limiter.h"

namespace ferry
{

/*
 * Map a request URI to a file path under `root`: strip query/fragment,
 * percent-decode, then normalize segments, rejecting NUL bytes and any
 * ".." that would escape the root (returns false).
 */
bool uri_to_safe_path(const std::string& root, const char *uri,
					  std::string *out);

/*
 * The request pipeline:
 *   client IP (XFF/peer) -> ACL -> path safety -> stat
 *   -> HEAD shortcut -> If-Range -> Range decision (cap/threshold)
 *   -> rate limiter (timer soft shaping / 429) -> pread series
 */
class Handler
{
public:
	Handler(const ServerConfig& cfg, std::shared_ptr<Acl> acl,
			std::shared_ptr<RateLimiter> limiter);

	void process(WFHttpTask *server_task);

	const ServerConfig& config() const { return this->cfg_; }

private:
	ServerConfig cfg_;					/* root normalized (no trailing '/') */
	std::shared_ptr<Acl> acl_;			/* may be null: ACL disabled */
	std::shared_ptr<RateLimiter> limiter_;	/* may be null: disabled */
};

} // namespace ferry

#endif // WF_HANDLER_H
