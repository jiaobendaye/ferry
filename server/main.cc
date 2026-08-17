#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "workflow/WFHttpServer.h"
#include "workflow/WFFacilities.h"
#include "workflow/WFTaskFactory.h"
#include "workflow/WFServer.h"

#include "acl.h"
#include "config.h"
#include "handler.h"
#include "rate_limiter.h"

using namespace ferry;

static WFFacilities::WaitGroup wait_group(1);

static void sig_handler(int signo)
{
	wait_group.done();
}

/*
 * Self-rescheduling periodic task on the workflow scheduler. Ticks every
 * second and runs `work` every `interval_sec` ticks, so shutdown (which
 * waits for the chain to observe `stop`) is bounded by ~1 s regardless
 * of the work interval.
 */
struct PeriodicTask
{
	std::function<void()> work;
	int interval_sec;
	std::atomic<bool> stop{false};
	std::atomic<int> ticks{0};
	WFFacilities::WaitGroup done_group{1};
};

static void arm_periodic(const std::shared_ptr<PeriodicTask>& ctx)
{
	WFTimerTask *timer = WFTaskFactory::create_timer_task(
					1, 0,
					[ctx](WFTimerTask *) {
		if (ctx->stop.load())
		{
			ctx->done_group.done();
			return;
		}
		if (++ctx->ticks >= ctx->interval_sec)
		{
			ctx->ticks = 0;
			ctx->work();
		}
		arm_periodic(ctx);
	});
	timer->start();
}

int main(int argc, char *argv[])
{
	const char *config_path = (argc >= 2 ? argv[1] : "config/server.conf");

	ServerConfig cfg;
	try
	{
		cfg = load_config(config_path);
	}
	catch (const std::exception& e)
	{
		fprintf(stderr, "%s\n", e.what());
		return 1;
	}

	std::shared_ptr<Acl> acl;
	if (!cfg.acl_file.empty())
	{
		try
		{
			acl = std::make_shared<Acl>(cfg.acl_file);
		}
		catch (const std::exception& e)
		{
			fprintf(stderr, "%s\n", e.what());
			return 1;
		}
	}

	std::shared_ptr<RateLimiter> limiter;
	if (cfg.rate_bytes_per_sec > 0)
		limiter = std::make_shared<RateLimiter>(cfg.rate_bytes_per_sec,
												cfg.max_wait_sec);

	auto handler = std::make_shared<Handler>(cfg, acl, limiter);

	struct WFServerParams params = HTTP_SERVER_PARAMS_DEFAULT;
	params.max_connections = cfg.max_connections;

	WFHttpServer server(&params,
			[handler](WFHttpTask *server_task) {
		handler->process(server_task);
	});

	if (server.start(cfg.port) < 0)
	{
		perror("start server");
		return 1;
	}

	/* periodic tasks: ACL hot reload + limiter idle sweep */
	std::shared_ptr<PeriodicTask> acl_timer;
	if (acl && cfg.acl_poll_interval_sec > 0)
	{
		acl_timer = std::make_shared<PeriodicTask>();
		acl_timer->interval_sec = cfg.acl_poll_interval_sec;
		acl_timer->work = [acl]() { acl->reload_if_changed(); };
		arm_periodic(acl_timer);
	}

	std::shared_ptr<PeriodicTask> sweep_timer;
	if (limiter)
	{
		sweep_timer = std::make_shared<PeriodicTask>();
		sweep_timer->interval_sec = 60;
		sweep_timer->work = [limiter]() {
			limiter->sweep(std::chrono::minutes(5));
		};
		arm_periodic(sweep_timer);
	}

	fprintf(stderr,
			"ferry-server listening: port=%u root=%s cap=%lld threshold=%lld "
			"rate=%lld B/s max_wait=%ds trust_hops=%d acl=%s(%zu black/%zu white) "
			"max_connections=%d\n",
			cfg.port, cfg.root.c_str(), cfg.cap_bytes, cfg.threshold(),
			cfg.rate_bytes_per_sec, cfg.max_wait_sec, cfg.trust_hops,
			cfg.acl_file.empty() ? "off" : cfg.acl_file.c_str(),
			acl ? acl->blacklist_size() : 0,
			acl ? acl->whitelist_size() : 0,
			cfg.max_connections);

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	wait_group.wait();

	if (acl_timer)
	{
		acl_timer->stop.store(true);
		acl_timer->done_group.wait();
	}
	if (sweep_timer)
	{
		sweep_timer->stop.store(true);
		sweep_timer->done_group.wait();
	}

	server.stop();
	fprintf(stderr, "ferry-server stopped\n");
	return 0;
}
