#include <string>

#include "probe.h"

namespace ferry
{

/*
 * The complete probe decision matrix (design D3).
 *
 * HEAD usable (200 + Content-Length):
 *   Accept-Ranges: bytes            -> CHUNK, probe GET not needed
 *   no Accept-Ranges, probe 206     -> CHUNK
 *   no Accept-Ranges, probe 200     -> size vs single_stream_limit:
 *                                      SINGLE_STREAM (the probe body is the
 *                                      start of the download) or
 *                                      REFUSE_OVERSIZE
 *   no Accept-Ranges, probe missing -> FAILED (decision impossible)
 *
 * HEAD not usable (failure, 405, no Content-Length): rely on the probe.
 *   probe 206 -> CHUNK (known_size is the Content-Range total, -1 when
 *                the server omitted it)
 *   probe 200 -> SINGLE_STREAM with the size limit check, or
 *                REFUSE_OVERSIZE; unknown size (-1) still enters
 *                SINGLE_STREAM and the engine enforces the limit mid-stream
 *   anything else -> FAILED
 */
ProbeDecision decide_mode(const HeadResult& head, const ProbeResult& probe,
						  long long single_stream_limit)
{
	ProbeDecision d;

	d.last_modified = head.last_modified;

	bool head_usable = head.got_response && head.status == 200 &&
					   head.content_length >= 0;

	if (head_usable)
	{
		if (head.accept_ranges)
		{
			/* full information: chunk mode, probe GET skipped */
			d.mode = DownloadMode::CHUNK;
			d.known_size = head.content_length;
			return d;
		}

		/* no Accept-Ranges: only the probe GET can decide */
		if (!probe.performed || !probe.got_response)
			return d;						/* FAILED (default) */

		if (probe.status == 206)
		{
			d.mode = DownloadMode::CHUNK;
			d.known_size = head.content_length;
			return d;
		}

		if (probe.status == 200)
		{
			if (head.content_length > single_stream_limit)
				d.mode = DownloadMode::REFUSE_OVERSIZE;
			else
			{
				d.mode = DownloadMode::SINGLE_STREAM;
				d.probe_body_is_download_start = true;
			}

			d.known_size = head.content_length;
			return d;
		}

		return d;							/* unexpected probe status */
	}

	/* HEAD not usable: rely on the probe */
	if (!probe.performed || !probe.got_response)
		return d;							/* FAILED (default) */

	if (probe.status == 206)
	{
		d.mode = DownloadMode::CHUNK;
		d.known_size = probe.content_length;	/* Content-Range total, -1
												 * when omitted */
		return d;
	}

	if (probe.status == 200)
	{
		long long size = probe.content_length;

		/* size -1 (unknown) never exceeds the limit: the engine enforces
		 * it mid-stream */
		if (size > single_stream_limit)
			d.mode = DownloadMode::REFUSE_OVERSIZE;
		else
		{
			d.mode = DownloadMode::SINGLE_STREAM;
			d.probe_body_is_download_start = true;
		}

		d.known_size = size;
		return d;
	}

	return d;								/* anything else: FAILED */
}

/* ---------------- network requests (HEAD / probe GET) ---------------- */

// network layer added by engine integration

} // namespace ferry
