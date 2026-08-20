#include <string>
#include <vector>

#include "path.h"

namespace ferry
{

static bool hex_digit(char c, int *v)
{
	if (c >= '0' && c <= '9')
		*v = c - '0';
	else if (c >= 'a' && c <= 'f')
		*v = c - 'a' + 10;
	else if (c >= 'A' && c <= 'F')
		*v = c - 'A' + 10;
	else
		return false;
	return true;
}

static bool percent_decode(const std::string& in, std::string *out)
{
	out->clear();
	for (size_t i = 0; i < in.size(); i++)
	{
		char c = in[i];
		if (c == '%')
		{
			int hi, lo;
			if (i + 2 >= in.size() ||
				!hex_digit(in[i + 1], &hi) || !hex_digit(in[i + 2], &lo))
				return false;
			out->push_back((char)((hi << 4) | lo));
			i += 2;
		}
		else
			out->push_back(c);
	}
	return true;
}

bool uri_to_safe_path(const std::string& root, const char *uri,
					  std::string *out)
{
	std::string path_part(uri);
	size_t q = path_part.find_first_of("?#");
	if (q != std::string::npos)
		path_part.erase(q);

	std::string decoded;
	if (!percent_decode(path_part, &decoded))
		return false;
	if (decoded.find('\0') != std::string::npos)
		return false;

	std::vector<std::string> segs;
	size_t pos = 0;

	while (pos <= decoded.size())
	{
		size_t slash = decoded.find('/', pos);
		size_t end = (slash == std::string::npos ? decoded.size() : slash);
		std::string seg = decoded.substr(pos, end - pos);

		if (seg == "..")
		{
			if (segs.empty())
				return false;
			segs.pop_back();
		}
		else if (!seg.empty() && seg != ".")
			segs.push_back(seg);

		if (slash == std::string::npos)
			break;
		pos = slash + 1;
	}

	std::string clean;
	for (const std::string& s : segs)
		clean += "/" + s;

	*out = root + clean;
	return true;
}

} // namespace ferry
