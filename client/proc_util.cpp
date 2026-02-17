#include "proc_util.h"
#include <algorithm>
#include <fstream>
#include <filesystem>

#include <cstring>
#include "../tools/masked_bmh.h"

#include <sys/uio.h>
#include <unistd.h>

bool ProcUtil::IsChildOf(pid_t child_pid, pid_t test_parent)
{
    auto pid = child_pid;
    // walk up the parent chain but avoid an infinite loop; limit depth
    const int max_depth = 128;
    for (int depth = 0; depth < max_depth; ++depth)
    {
        std::ifstream fi { "/proc/"+std::to_string(pid)+"/stat" };
        if (!fi)
            break;

        pid_t _pid;
        std::string name;
        char state;
        pid_t parent;
        fi >> _pid >> name >> state >> parent;

        if (parent == test_parent)
            return true;
        if (parent <= 1 || parent == pid)
            break;

        pid = parent;
    }
    return false;
}

std::vector<int> ProcUtil::FindProcsByName(const std::string &pattern)
{
    std::vector<int> result;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/"))
    {
        const std::string &path_name = entry.path().filename().string();

        pid_t pid = atoi(path_name.c_str());

        if (!entry.is_directory() || !pid)
            continue;

        auto cmd_path = entry.path() / "cmdline";

        if (std::ifstream cmdline_f { cmd_path.string(), std::ios::binary})
        {
            std::string contents((std::istreambuf_iterator<char>(cmdline_f)), std::istreambuf_iterator<char>());

            std::replace_if(contents.begin(), contents.end(), [] (char c) { return c == 0; }, ' ');

            if (contents.find(pattern) != std::string::npos)
                result.push_back(pid);
        }
    }

    return result;
}

bool ProcUtil::ProcessExists(pid_t pid)
{
    return std::filesystem::exists("/proc/"+std::to_string(pid));
}

size_t ProcUtil::ReadMemoryBytes(pid_t pid, uintptr_t address, void *dest, uint64_t size)
{
    iovec local_addr { dest, size };
    iovec remote_addr { reinterpret_cast<void *>(address), size };

    return process_vm_readv(pid, &local_addr, 1, &remote_addr, 1, 0 );
}

size_t ProcUtil::WriteMemoryBytes(pid_t pid, uintptr_t address, void *dest, uint64_t size)
{
    iovec local_addr { dest, size };
    iovec remote_addr { reinterpret_cast<void *>(address), size };
    return process_vm_writev(pid, &local_addr, 1, &remote_addr, 1, 0 );
}

std::vector<ProcUtil::MemPage> ProcUtil::GetPages(pid_t pid, const std::string &name)
{
    std::vector<MemPage> pages;

    if (std::ifstream fi{"/proc/"+std::to_string(pid)+"/maps"})
    {
        std::string line, filename;
        uintptr_t size;
        uintptr_t start, end;
        char read, write, exec, cow, _;
        uint32_t offset, dev_major, dev_minor, inode;

        while (std::getline(fi, line))
        {
            filename.resize(line.size());
            if (sscanf(line.c_str(), "%lx-%lx %c%c%c%c %x %x:%x %u %[^\n]",
                &start, &end,
                &read,&write, &exec, &cow,
                &offset,
                &dev_major, &dev_minor,
                &inode, &filename[0]) >= 6)
            {
                if (name.length() && filename.find(name) == std::string::npos)
                {
                    continue;
                }
                pages.emplace_back(start, end, read, write, exec, cow, offset, size, filename);
            }
        }
    }
    return pages;
}

uint64_t ProcUtil::GetMemoryUsage(pid_t pid)
{
    if (std::ifstream fi { "/proc/"+std::to_string(pid)+"/stat" })
    {
        std::string _pid, comm, state, ppid, pgrp, session, tty_nr;
        std::string tpgid, flags, minflt, cminflt, majflt, cmajflt;
        std::string utime, stime, cutime, cstime, priority, nice;
        std::string O, itrealvalue, starttime;

        // the two fields we want
        uint64_t vsize;
        int64_t rss;

        fi >> _pid >> comm >> state >> ppid >> pgrp >> session >> tty_nr
                    >> tpgid >> flags >> minflt >> cminflt >> majflt >> cmajflt
                    >> utime >> stime >> cutime >> cstime >> priority >> nice
                    >> O >> itrealvalue >> starttime >> vsize >> rss; // don't care about the rest

        int64_t page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024; // in case x86-64 is configured to use 2MB pages
        auto vm_usage     = vsize / 1024;
        auto resident_set = rss * page_size_kb;
        return resident_set;
    }
    return 0;
}

int ProcUtil::QueryMemory(pid_t pid, unsigned char *query, const char *mask, uintptr_t *out, uint32_t amount)
{
    if (!query || !mask || !out || amount == 0)
        return 0;

    const size_t query_size = std::strlen(mask);
    if (query_size == 0)
        return 0;

    uint32_t finds = 0;
    const uint32_t alignment = 1;

    std::vector<uint8_t> buffer; // reused per region
    const uint8_t *query_bytes = reinterpret_cast<const uint8_t *>(query);

    for (const auto &region : GetPages(pid))
    {
        if (finds == amount) break;

        const size_t region_size = region.end - region.start;
        if (query_size > region_size) continue;

        buffer.resize(region_size);
        const ssize_t bytes_read = ReadMemoryBytes(pid, region.start, buffer.data(), region_size);
        if (bytes_read < static_cast<ssize_t>(query_size)) continue;

        size_t offset = 0;
        const size_t readable = static_cast<size_t>(bytes_read);

        while (finds != amount)
        {
            const size_t found = masked_bmh_search(
                buffer.data(),
                readable,
                query_bytes,
                mask,
                query_size,
                offset,
                alignment);

            if (found == SIZE_MAX) break;

            out[finds++] = region.start + found;
            offset = found + 1;
            if (offset + query_size > readable) break;
        }
    }

    return static_cast<int>(finds);
}

uintptr_t ProcUtil::FindPattern(pid_t pid, const std::string &query, const std::string &segment)
{
    std::stringstream ss(query);
    std::string data{ };
    std::string mask{ };
    std::vector<uint8_t> bytes;
    uintptr_t result = 0;

    while (std::getline(ss, data, ' '))
    {
        if (data.find('?') != std::string::npos)
        {
            mask += "?";
            bytes.push_back(0);
        }
        else
        {
            bytes.push_back(static_cast<uint8_t>(std::stoi(data, nullptr, 16)));
            mask += "x";
        }
    }

    QueryMemory(pid, &bytes.at(0), mask.c_str(), &result, 1);

    return result;
}

pid_t ProcUtil::GetParent(pid_t pid)
{
    if (std::ifstream fi { "/proc/"+std::to_string(pid)+"/stat" } )
    {
        pid_t pid;
        std::string name;
        char state;
        int parent;
        fi >> pid >> name >> state >> parent;
        return parent;
    }
    return 0;
}
