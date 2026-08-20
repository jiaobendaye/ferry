#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include "range.h"

namespace ferry
{

static std::string trim(const std::string& s)
{
	size_t b = s.find_first_not_of(" \t");
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t");
	return s.substr(b, e - b + 1);
}

static bool parse_nonneg(const std::string& s, long long *out)
{
	if (s.empty())
		return false;

	const char *p = s.c_str();
	char *end = NULL;

	errno = 0;
	long long v = strtoll(p, &end, 10);
	if (errno != 0 || end == p || *end != '\0' || v < 0)
		return false;

	*out = v;
	return true;
}

static bool iequals(const std::string& a, const char *b)
{
	size_t n = a.size();
	if (n != strlen(b))
		return false;
	for (size_t i = 0; i < n; i++)
		if (tolower((unsigned char)a[i]) != (unsigned char)b[i])
			return false;
	return true;
}

/*
 * Parse one range-spec ("first-last" / "first-" / "-suffix").
 * Returns false when the spec is syntactically invalid.
 * kind: 0 = explicit, 1 = suffix.
 */
static bool parse_spec(const std::string& spec, int *kind,
					   long long *first, long long *last)
{
	size_t dash = spec.find('-');
	if (dash == std::string::npos)
		return false;

	std::string lhs = trim(spec.substr(0, dash));
	std::string rhs = trim(spec.substr(dash + 1));

	if (lhs.empty())					/* suffix range "-N" */
	{
		long long suffix;
		if (!parse_nonneg(rhs, &suffix))
			return false;
		*kind = 1;
		*first = suffix;				/* suffix length */
		*last = 0;
		return true;
	}

	long long f;
	if (!parse_nonneg(lhs, &f))
		return false;

	if (rhs.empty())					/* open-ended "N-" */
	{
		*kind = 0;
		*first = f;
		*last = -1;						/* to end of file */
		return true;
	}

	long long l;
	if (!parse_nonneg(rhs, &l))
		return false;
	if (l < f)							/* first > last: invalid */
		return false;

	*kind = 0;
	*first = f;
	*last = l;
	return true;
}

RangeDecision decide_range(const std::string& range_header,
						   long long file_size,
						   long long cap_bytes,
						   long long threshold_bytes)
{
	RangeDecision d;
	std::string header = trim(range_header);

	bool have_range = false;
	int kind = 0;
	long long first = 0, last = 0;

	if (!header.empty())
	{
		size_t eq = header.find('=');
		if (eq != std::string::npos &&
			iequals(trim(header.substr(0, eq)), "bytes"))
		{
			std::string specs = header.substr(eq + 1);
			size_t comma = specs.find(',');	/* keep first range only */
			std::string spec = trim(comma == std::string::npos ?
									specs : specs.substr(0, comma));

			if (parse_spec(spec, &kind, &first, &last))
				have_range = true;
			/* invalid spec: header ignored (non-Range rules below) */
		}
	}

	if (!have_range)
	{
		if (file_size > threshold_bytes)
		{
			d.status = 413;
			return d;
		}
		d.status = 200;
		d.offset = 0;
		d.length = file_size;
		return d;
	}

	long long start, end;

	if (kind == 1)						/* suffix: last N bytes */
	{
		if (first <= 0)					/* "bytes=-0": unsatisfiable */
		{
			d.status = 416;
			return d;
		}
		if (first > file_size)
			first = file_size;
		start = file_size - first;
		end = file_size - 1;
	}
	else
	{
		start = first;
		if (start >= file_size)
		{
			d.status = 416;
			return d;
		}
		end = (last < 0 || last >= file_size) ? file_size - 1 : last;
	}

	/* Truncate to the cap (RFC 9110 permits a shorter range). */
	if (end - start + 1 > cap_bytes)
		end = start + cap_bytes - 1;

	d.status = 206;
	d.offset = start;
	d.length = end - start + 1;
	return d;
}

} // namespace ferry
