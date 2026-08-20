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
#include "gates.h"
#include "handler.h"
#include "stats.h"

using namespace ferry;

static WFFacilities::WaitGroup wait_group(1);
static std::atomic<bool> stats_dump_requested(false);

static void sig_handler(int signo)
{
	wait_group.done();
}

/* Signal-safe: only sets a flag; the 1 s stats tick does the printing. */
static void usr1_handler(int signo)
{
	stats_dump_requested.store(true);
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

	auto stats = std::make_shared<Stats>();
	GateSetup gates = build_gate_chains(cfg);

	auto handler = std::make_shared<Handler>(cfg, acl,
											 gates.pre, gates.post, stats);

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
	if (gates.qps_per_ip_limiter || gates.bw_per_ip_limiter)
	{
		sweep_timer = std::make_shared<PeriodicTask>();
		sweep_timer->interval_sec = 60;
		auto qps = gates.qps_per_ip_limiter;
		auto bw = gates.bw_per_ip_limiter;
		sweep_timer->work = [qps, bw]() {
			if (qps)
				qps->sweep(std::chrono::minutes(5));
			if (bw)
				bw->sweep(std::chrono::minutes(5));
		};
		arm_periodic(sweep_timer);
	}

	/* stats ticker: 1 s tick; prints on SIGUSR1 and/or every interval.
	   Armed unconditionally so SIGUSR1 works with periodic stats off. */
	auto prev_snapshot = std::make_shared<Stats::Snapshot>();
	std::shared_ptr<PeriodicTask> stats_timer = std::make_shared<PeriodicTask>();
	stats_timer->interval_sec = 1;
	{
		int interval = cfg.stats_interval_sec;
		auto tick_count = std::make_shared<int>(0);
		auto qps_limiter = gates.qps_per_ip_limiter;
		auto bw_limiter = gates.bw_per_ip_limiter;

		stats_timer->work = [stats, prev_snapshot, tick_count, interval,
							 qps_limiter, bw_limiter]() {
			bool dump = stats_dump_requested.exchange(false);

			if (!dump && interval > 0 && ++(*tick_count) % interval == 0)
				dump = true;
			if (!dump)
				return;

			Stats::Snapshot cur = stats->snapshot();
			long long qb = qps_limiter ? (long long)qps_limiter->size() : 0;
			long long bb = bw_limiter ? (long long)bw_limiter->size() : 0;

			fprintf(stderr, "%s\n",
					format_stats_line(cur, *prev_snapshot, qb, bb).c_str());
			*prev_snapshot = cur;
		};
	}
	arm_periodic(stats_timer);

	fprintf(stderr,
			"ferry-server listening: port=%u root=%s file_body=%s "
			"cap=%lld threshold=%lld "
			"rate=%lld B/s rate_total=%lld B/s max_wait=%ds trust_hops=%d "
			"acl=%s(%zu black/%zu white) max_connections=%d "
			"qps_total=%lld qps_per_ip=%lld max_inflight=%d "
			"max_inflight_per_ip=%d stats_interval=%ds\n",
			cfg.port, cfg.root.c_str(), file_body_mode_name(cfg.file_body_mode),
			cfg.cap_bytes, cfg.threshold(),
			cfg.rate_bytes_per_sec, cfg.rate_total_bps, cfg.max_wait_sec,
			cfg.trust_hops,
			cfg.acl_file.empty() ? "off" : cfg.acl_file.c_str(),
			acl ? acl->blacklist_size() : 0,
			acl ? acl->whitelist_size() : 0,
			cfg.max_connections,
			cfg.qps_total, cfg.qps_per_ip, cfg.max_inflight,
			cfg.max_inflight_per_ip, cfg.stats_interval_sec);

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGUSR1, usr1_handler);

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
	stats_timer->stop.store(true);
	stats_timer->done_group.wait();

	server.stop();
	fprintf(stderr, "ferry-server stopped\n");
	return 0;
}
