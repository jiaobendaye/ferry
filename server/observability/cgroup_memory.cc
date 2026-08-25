#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include "cgroup_memory.h"

namespace ferry
{

static bool read_text_file(const std::string& path, std::string *out)
{
	std::ifstream in(path);
	if (!in)
		return false;
	std::ostringstream buffer;
	buffer << in.rdbuf();
	*out = buffer.str();
	return in.good() || in.eof();
}

CgroupMemoryReader::CgroupMemoryReader(ReadFileFn read_file)
	: read_file_(std::move(read_file))
{
	if (!this->read_file_)
		this->read_file_ = read_text_file;
}

std::string CgroupMemoryReader::decode_proc_path(const std::string& path)
{
	std::string decoded;
	decoded.reserve(path.size());
	for (size_t i = 0; i < path.size(); i++)
	{
		if (path[i] == '\\' && i + 3 < path.size() &&
			path[i + 1] >= '0' && path[i + 1] <= '7' &&
			path[i + 2] >= '0' && path[i + 2] <= '7' &&
			path[i + 3] >= '0' && path[i + 3] <= '7')
		{
			int value = (path[i + 1] - '0') * 64 +
						(path[i + 2] - '0') * 8 + path[i + 3] - '0';
			decoded.push_back((char)value);
			i += 3;
		}
		else
			decoded.push_back(path[i]);
	}
	return decoded;
}

static bool path_below_root(const std::string& path, const std::string& root,
							std::string *relative)
{
	if (root == "/")
	{
		*relative = path;
		return !path.empty() && path[0] == '/';
	}
	if (path == root)
	{
		*relative = "/";
		return true;
	}
	if (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
		path[root.size()] == '/')
	{
		*relative = path.substr(root.size());
		return true;
	}
	return false;
}

bool CgroupMemoryReader::resolve_memory_stat_path(
									const std::string& cgroup_text,
									const std::string& mountinfo_text,
									std::string *path)
{
	std::string unified;
	std::istringstream cgroups(cgroup_text);
	std::string line;
	while (std::getline(cgroups, line))
	{
		if (line.compare(0, 3, "0::") == 0)
		{
			unified = decode_proc_path(line.substr(3));
			break;
		}
	}
	if (unified.empty() || unified[0] != '/')
		return false;

	std::istringstream mounts(mountinfo_text);
	while (std::getline(mounts, line))
	{
		size_t separator = line.find(" - ");
		if (separator == std::string::npos)
			continue;

		std::istringstream right(line.substr(separator + 3));
		std::string fs_type;
		right >> fs_type;
		if (fs_type != "cgroup2")
			continue;

		std::istringstream left(line.substr(0, separator));
		std::vector<std::string> fields;
		std::string field;
		while (left >> field)
			fields.push_back(field);
		if (fields.size() < 5)
			continue;

		std::string root = decode_proc_path(fields[3]);
		std::string mount_point = decode_proc_path(fields[4]);
		std::string relative;
		if (!path_below_root(unified, root, &relative))
			continue;
		while (mount_point.size() > 1 && mount_point.back() == '/')
			mount_point.pop_back();
		if (relative == "/")
			*path = mount_point + "/memory.stat";
		else
			*path = mount_point + relative + "/memory.stat";
		return true;
	}
	return false;
}

static long long parse_nonnegative(const std::string& value)
{
	if (value.empty())
		return -1;
	char *end = nullptr;
	errno = 0;
	long long parsed = strtoll(value.c_str(), &end, 10);
	if (errno != 0 || end == value.c_str() || *end != '\0' || parsed < 0)
		return -1;
	return parsed;
}

CgroupMemorySnapshot CgroupMemoryReader::parse_memory_stat(
												 const std::string& text)
{
	CgroupMemorySnapshot snapshot;
	std::istringstream input(text);
	std::string key, value;
	while (input >> key >> value)
	{
		long long parsed = parse_nonnegative(value);
		if (key == "anon")
			snapshot.anon = parsed;
		else if (key == "file")
			snapshot.file = parsed;
		else if (key == "sock")
			snapshot.sock = parsed;
	}
	return snapshot;
}

CgroupMemorySnapshot CgroupMemoryReader::snapshot() const
{
	std::string cgroup_text, mountinfo_text, memory_text, path;
	if (!this->read_file_("/proc/self/cgroup", &cgroup_text) ||
		!this->read_file_("/proc/self/mountinfo", &mountinfo_text) ||
		!resolve_memory_stat_path(cgroup_text, mountinfo_text, &path) ||
		!this->read_file_(path, &memory_text))
		return CgroupMemorySnapshot();
	return parse_memory_stat(memory_text);
}

} // namespace ferry
