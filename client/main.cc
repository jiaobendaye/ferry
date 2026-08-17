#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <exception>
#include <string>

#include "cli.h"
#include "engine.h"
#include "progress.h"
#include "verify.h"

int main(int argc, char *argv[])
{
	ferry::ClientConfig cfg;

	try
	{
		cfg = ferry::parse_args(argc, argv);
	}
	catch (const std::exception& e)
	{
		fprintf(stderr, "ferry-client: %s\n\n%s", e.what(),
				ferry::usage_text().c_str());
		return 2;
	}

	if (cfg.show_help)
	{
		fputs(ferry::usage_text().c_str(), stdout);
		return 0;
	}

	if (cfg.output.empty())
		cfg.output = ferry::basename_of_url(cfg.url);

	std::string part = cfg.output + ".part";
	std::string meta = cfg.output + ".ferry.json";

	fprintf(stderr, "ferry-client: %s -> %s (jobs=%d, chunk=%s)\n",
			cfg.url.c_str(), cfg.output.c_str(), cfg.jobs,
			ferry::format_bytes(cfg.chunk_size).c_str());

	auto t0 = std::chrono::steady_clock::now();
	ferry::EngineOutcome out = ferry::run_download(cfg, part, meta);
	auto t1 = std::chrono::steady_clock::now();
	long long elapsed_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

	if (out.interrupted)
	{
		fprintf(stderr, "interrupted: state saved in %s / %s\n"
						"run the same command to resume\n",
				part.c_str(), meta.c_str());
		return 130;
	}

	if (!out.success)
	{
		fprintf(stderr, "ferry-client: %s\n", out.error.c_str());
		return 1;
	}

	/* ---- final verification gates the output file ---- */
	std::string digest;

	if (!cfg.no_verify)
	{
		digest = ferry::sha256_of_file(part);
		if (digest.empty())
		{
			fprintf(stderr, "ferry-client: verification read failed\n");
			return 1;
		}

		if (!cfg.checksum.empty() &&
			!ferry::checksum_spec_matches("sha-256=" + cfg.checksum, digest))
		{
			fprintf(stderr,
					"ferry-client: checksum MISMATCH\n"
					"  expected sha-256=%s\n"
					"  actual   sha-256=%s\n"
					"files kept: %s / %s\n",
					cfg.checksum.c_str(), digest.c_str(),
					part.c_str(), meta.c_str());
			return 1;
		}
	}

	if (rename(part.c_str(), cfg.output.c_str()) != 0)
	{
		perror("ferry-client: rename part file");
		return 1;
	}
	remove(meta.c_str());

	double avg = (elapsed_ms > 0 && out.total_bytes > 0) ?
				 (double)out.total_bytes / ((double)elapsed_ms / 1000.0) : 0;
	std::string summary = ferry::format_summary(out.total_bytes, elapsed_ms,
												avg, digest);
	fprintf(stderr, "%s%s\n", summary.c_str(),
			cfg.no_verify ? " (verification skipped)" : "");
	return 0;
}
