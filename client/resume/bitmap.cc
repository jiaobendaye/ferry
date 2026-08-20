#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include "bitmap.h"

namespace ferry
{

ChunkBitmap::ChunkBitmap(long long count)
{
	this->reset(count);
}

void ChunkBitmap::reset(long long count)
{
	if (count < 0)
		count = 0;
	this->count_ = count;
	this->bits_.assign((size_t)((count + 7) / 8), 0);
}

void ChunkBitmap::mark(long long idx)
{
	if (idx < 0 || idx >= this->count_)
		return;
	this->bits_[(size_t)(idx / 8)] |= (unsigned char)(1 << (idx % 8));
}

bool ChunkBitmap::test(long long idx) const
{
	if (idx < 0 || idx >= this->count_)
		return false;
	return (this->bits_[(size_t)(idx / 8)] & (1 << (idx % 8))) != 0;
}

long long ChunkBitmap::count() const
{
	return this->count_;
}

static long long popcount8(unsigned char b)
{
	long long n = 0;
	while (b)
	{
		n += b & 1;
		b >>= 1;
	}
	return n;
}

long long ChunkBitmap::done() const
{
	long long n = 0;
	for (unsigned char b : this->bits_)
		n += popcount8(b);
	return n;
}

std::string ChunkBitmap::serialize() const
{
	static const char digits[] = "0123456789abcdef";

	std::string hex;
	hex.reserve(this->bits_.size() * 2);
	for (unsigned char b : this->bits_)
	{
		hex.push_back(digits[b >> 4]);
		hex.push_back(digits[b & 0x0F]);
	}
	return hex;
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

bool ChunkBitmap::deserialize(const std::string& hex, long long count)
{
	if (count < 0)
		return false;

	size_t nbytes = (size_t)((count + 7) / 8);
	if (hex.size() != nbytes * 2)
		return false;

	std::vector<unsigned char> bits(nbytes, 0);
	for (size_t i = 0; i < nbytes; i++)
	{
		int hi = hex_value(hex[2 * i]);
		int lo = hex_value(hex[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return false;
		bits[i] = (unsigned char)((hi << 4) | lo);
	}

	/* bits beyond the last chunk must be clear, or done() would lie */
	if (nbytes > 0 && count % 8 != 0)
	{
		unsigned char mask = (unsigned char)(0xFFU << (count % 8));
		if (bits[nbytes - 1] & mask)
			return false;
	}

	this->count_ = count;
	this->bits_ = std::move(bits);
	return true;
}

/*
 * Meta JSON, one line, fixed key set:
 * {"version":1,"url":"...","size":N,"last_modified":"...",
 *  "chunk_size":N,"chunk_count":N,"bitmap":"hex"}
 * Only backslash and quote are escaped — that is all an URL or an HTTP
 * date can need.
 */

static std::string json_escape(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s)
	{
		if (c == '\\' || c == '"')
			out.push_back('\\');
		out.push_back(c);
	}
	return out;
}

std::string meta_to_json(const DownloadMeta& meta)
{
	std::string json;
	json += "{\"version\":" + std::to_string(meta.version);
	json += ",\"url\":\"" + json_escape(meta.url) + "\"";
	json += ",\"size\":" + std::to_string(meta.size);
	json += ",\"last_modified\":\"" + json_escape(meta.last_modified) + "\"";
	json += ",\"chunk_size\":" + std::to_string(meta.chunk_size);
	json += ",\"chunk_count\":" + std::to_string(meta.bitmap.count());
	json += ",\"bitmap\":\"" + meta.bitmap.serialize() + "\"";
	json += "}";
	return json;
}

enum meta_field
{
	META_VERSION		= 0,
	META_URL			= 1,
	META_SIZE			= 2,
	META_LAST_MODIFIED	= 3,
	META_CHUNK_SIZE		= 4,
	META_CHUNK_COUNT	= 5,
	META_BITMAP			= 6,
	META_FIELD_COUNT	= 7
};

static int field_by_name(const std::string& key)
{
	if (key == "version")
		return META_VERSION;
	if (key == "url")
		return META_URL;
	if (key == "size")
		return META_SIZE;
	if (key == "last_modified")
		return META_LAST_MODIFIED;
	if (key == "chunk_size")
		return META_CHUNK_SIZE;
	if (key == "chunk_count")
		return META_CHUNK_COUNT;
	if (key == "bitmap")
		return META_BITMAP;
	return -1;
}

static void skip_ws(const std::string& s, size_t *pos)
{
	while (*pos < s.size() &&
		   (s[*pos] == ' ' || s[*pos] == '\t' ||
			s[*pos] == '\n' || s[*pos] == '\r'))
		(*pos)++;
}

static bool consume(const std::string& s, size_t *pos, char c)
{
	if (*pos >= s.size() || s[*pos] != c)
		return false;
	(*pos)++;
	return true;
}

/*
 * Parse a JSON string starting at s[*pos]; *out is untouched unless the
 * parse succeeds. Only \\ and \" escapes are accepted — all the writer
 * ever emits.
 */
static bool parse_json_string(const std::string& s, size_t *pos,
							  std::string *out)
{
	if (!consume(s, pos, '"'))
		return false;

	std::string v;
	while (*pos < s.size())
	{
		char c = s[(*pos)++];
		if (c == '"')
		{
			*out = std::move(v);
			return true;
		}

		if (c == '\\')
		{
			if (*pos >= s.size())
				return false;
			char e = s[(*pos)++];
			if (e != '\\' && e != '"')
				return false;
			v.push_back(e);
		}
		else if ((unsigned char)c < 0x20)
			return false;		/* raw control char: not from our writer */
		else
			v.push_back(c);
	}

	return false;				/* unterminated string */
}

/* Parse a JSON number as a whole long long; trailing junk fails. */
static bool parse_json_int(const std::string& s, size_t *pos, long long *out)
{
	size_t start = *pos;

	if (*pos < s.size() && s[*pos] == '-')
		(*pos)++;

	if (*pos >= s.size() || !isdigit((unsigned char)s[*pos]))
		return false;
	while (*pos < s.size() && isdigit((unsigned char)s[*pos]))
		(*pos)++;

	std::string num = s.substr(start, *pos - start);
	char *end = NULL;

	errno = 0;
	long long v = strtoll(num.c_str(), &end, 10);
	if (errno != 0 || end != num.c_str() + num.size())
		return false;

	*out = v;
	return true;
}

/*
 * Strict parser for meta_to_json() output: every known key exactly once,
 * correct types, nothing unknown, nothing after the closing brace.
 */
bool meta_from_json(const std::string& json, DownloadMeta *out)
{
	DownloadMeta meta;
	long long version = 0;
	long long chunk_count = -1;
	std::string bitmap_hex;
	bool have[META_FIELD_COUNT] = { false };

	size_t pos = 0;
	skip_ws(json, &pos);
	if (!consume(json, &pos, '{'))
		return false;

	bool first = true;
	while (true)
	{
		skip_ws(json, &pos);
		if (consume(json, &pos, '}'))
			break;

		if (!first && !consume(json, &pos, ','))
			return false;
		first = false;

		skip_ws(json, &pos);
		std::string key;
		if (!parse_json_string(json, &pos, &key))
			return false;

		skip_ws(json, &pos);
		if (!consume(json, &pos, ':'))
			return false;
		skip_ws(json, &pos);

		int field = field_by_name(key);
		if (field < 0 || have[field])
			return false;		/* unknown or duplicated key */
		have[field] = true;

		switch (field)
		{
		case META_VERSION:
			if (!parse_json_int(json, &pos, &version))
				return false;
			break;
		case META_URL:
			if (!parse_json_string(json, &pos, &meta.url))
				return false;
			break;
		case META_SIZE:
			if (!parse_json_int(json, &pos, &meta.size))
				return false;
			break;
		case META_LAST_MODIFIED:
			if (!parse_json_string(json, &pos, &meta.last_modified))
				return false;
			break;
		case META_CHUNK_SIZE:
			if (!parse_json_int(json, &pos, &meta.chunk_size))
				return false;
			break;
		case META_CHUNK_COUNT:
			if (!parse_json_int(json, &pos, &chunk_count))
				return false;
			break;
		case META_BITMAP:
			if (!parse_json_string(json, &pos, &bitmap_hex))
				return false;
			break;
		}
	}

	skip_ws(json, &pos);
	if (pos != json.size())
		return false;			/* trailing garbage */

	for (int i = 0; i < META_FIELD_COUNT; i++)
	{
		if (!have[i])
			return false;		/* missing field */
	}

	if (version != 1 || meta.size < 0 || meta.chunk_size < 0 ||
		chunk_count < 0)
		return false;

	if (!meta.bitmap.deserialize(bitmap_hex, chunk_count))
		return false;

	*out = meta;
	return true;
}

bool save_meta_atomic(const std::string& path, const DownloadMeta& meta)
{
	std::string tmp = path + ".tmp." + std::to_string(getpid());
	std::string json = meta_to_json(meta);

	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;

		out.write(json.data(), (std::streamsize)json.size());
		out.flush();
		if (!out)
		{
			out.close();
			remove(tmp.c_str());
			return false;
		}
	}

	if (rename(tmp.c_str(), path.c_str()) < 0)
	{
		remove(tmp.c_str());
		return false;
	}
	return true;
}

bool load_meta(const std::string& path, DownloadMeta *out)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;

	std::ostringstream ss;
	ss << in.rdbuf();
	return meta_from_json(ss.str(), out);
}

} // namespace ferry
