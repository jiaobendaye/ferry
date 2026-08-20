#ifndef FERRY_ENGINE_H
#define FERRY_ENGINE_H

#include <string>
#include "ui/cli.h"

namespace ferry
{

struct EngineOutcome
{
	bool success = false;
	bool interrupted = false;		/* SIGINT: state kept, resumable */
	bool chunk_mode = true;			/* false: single-stream download */
	bool resumed = false;
	long long total_bytes = -1;		/* file size when known */
	long long retry_count = 0;		/* chunk retries performed */
	std::string last_modified;
	std::string error;				/* set when !success && !interrupted */
};

/*
 * Full download lifecycle: probe -> plan/resume -> workers -> completion.
 * Data lands at `part_path`; chunk-mode resume state at `meta_path`
 * ("" disables meta handling, never used for single-stream).
 *
 * Verification, rename and meta cleanup are the caller's job (main).
 * Installs a SIGINT handler for the duration of the call.
 */
EngineOutcome run_download(const ClientConfig& cfg,
						   const std::string& part_path,
						   const std::string& meta_path);

} // namespace ferry

#endif // FERRY_ENGINE_H
