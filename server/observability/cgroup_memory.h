#ifndef FERRY_CGROUP_MEMORY_H
#define FERRY_CGROUP_MEMORY_H

#include <functional>
#include <string>

namespace ferry
{

struct CgroupMemorySnapshot
{
	long long anon = -1;
	long long file = -1;
	long long sock = -1;
};

class CgroupMemoryReader
{
public:
	using ReadFileFn = std::function<bool(const std::string&, std::string *)>;

	explicit CgroupMemoryReader(ReadFileFn read_file = ReadFileFn());

	CgroupMemorySnapshot snapshot() const;

	static bool resolve_memory_stat_path(const std::string& cgroup_text,
											 const std::string& mountinfo_text,
											 std::string *path);
	static CgroupMemorySnapshot parse_memory_stat(const std::string& text);
	static std::string decode_proc_path(const std::string& path);

private:
	ReadFileFn read_file_;
};

} // namespace ferry

#endif // FERRY_CGROUP_MEMORY_H
