#include "proc_util.h"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <unordered_map>


#include <cstring>
#include "masked_bmh.h"

#include <fcntl.h>
#include <time.h>
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

// Concrete entry point; callers pass an initializer_list of patterns.
std::vector<int> ProcUtil::FindProcsByName(const std::initializer_list<std::string> &patterns)
{
    std::vector<int> result;

    // nothing to search for
    if (patterns.size() == 0)
        return result;

    for (const auto &entry : std::filesystem::directory_iterator("/proc/"))
    {
        const std::string &path_name = entry.path().filename().string();
        pid_t pid = atoi(path_name.c_str());

        if (!entry.is_directory() || !pid)
            continue;

        auto cmd_path = std::filesystem::path("/proc") / std::to_string(pid) / "cmdline";
        std::ifstream cmdline_f { cmd_path.string(), std::ios::binary };
        if (cmdline_f)
        {
            std::string contents((std::istreambuf_iterator<char>(cmdline_f)), std::istreambuf_iterator<char>());
            if (!contents.empty())
            {
                std::replace_if(contents.begin(), contents.end(), [] (char c) { return c == 0; }, ' ');
                bool match = true;
                for (const auto &pat : patterns)
                {
                    if (contents.find(pat) == std::string::npos)
                    {
                        match = false;
                        break;
                    }
                }
                if (match)
                    result.push_back(pid);
            }
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
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    char buf[512];
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    // Skip comm field (may contain spaces/parens): scan past last ')'
    const char *rp = strrchr(buf, ')');
    if (!rp || rp[1] == '\0') return 0;

    char state;
    int ppid, pgrp, session, tty_nr, tpgid;
    unsigned int flags;
    unsigned long minflt, cminflt, majflt, cmajflt, utime, stime;
    long cutime, cstime, priority, nice, num_threads, itrealvalue;
    unsigned long long starttime;
    unsigned long vsize;
    long rss;

    if (sscanf(rp + 2,
               "%c %d %d %d %d %d %u "
               "%lu %lu %lu %lu %lu %lu %ld %ld "
               "%ld %ld %ld %ld %llu "
               "%lu %ld",
               &state, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
               &minflt, &cminflt, &majflt, &cmajflt, &utime, &stime,
               &cutime, &cstime,
               &priority, &nice, &num_threads, &itrealvalue,
               &starttime,
               &vsize, &rss) != 22)
        return 0;

    const long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024;
    return static_cast<uint64_t>(rss) * static_cast<uint64_t>(page_size_kb);
}

double ProcUtil::GetCpuUsage(pid_t pid)
{
    struct CpuStat {
        uint64_t proc_ticks = 0;
        uint64_t start_time = 0;
        uint64_t last_ms    = 0;
        double   cached     = 0.0;
    };

    static thread_local std::unordered_map<pid_t, CpuStat> prev;

    static const double clk_tck = static_cast<double>(sysconf(_SC_CLK_TCK));
    static const double nproc   = static_cast<double>(sysconf(_SC_NPROCESSORS_ONLN));

    auto &p = prev[pid];

    // --- Time ---
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    const uint64_t now_ms =
        static_cast<uint64_t>(now.tv_sec) * 1000ull +
        static_cast<uint64_t>(now.tv_nsec) / 1'000'000ull;

    const uint64_t elapsed_ms = now_ms - p.last_ms;

    if (elapsed_ms < 250u)
        return p.cached;

    const double total_delta = static_cast<double>(elapsed_ms) * clk_tck / 1000.0;
    if (total_delta <= 0.0)
        return p.cached;

    // --- Read /proc/<pid>/stat ---
    uint64_t proc_ticks = 0;
    uint64_t start_time = 0;

    {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);

        char buf[1024]; // safer buffer
        int fd = ::open(path, O_RDONLY);
        if (fd < 0)
            return p.cached;

        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);

        if (n <= 0)
            return p.cached;

        buf[n] = '\0';

        const char *rp = strrchr(buf, ')');
        if (!rp || rp[1] == '\0')
            return p.cached;

        char state;
        int ppid, pgrp, session, tty_nr, tpgid;
        unsigned int flags;
        unsigned long minflt, cminflt, majflt, cmajflt, utime, stime;
        long cutime, cstime, priority, nice, num_threads, itrealvalue;
        unsigned long long starttime;

        if (sscanf(rp + 2,
                   "%c %d %d %d %d %d %u "
                   "%lu %lu %lu %lu %lu %lu %ld %ld "
                   "%ld %ld %ld %ld %llu",
                   &state, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
                   &minflt, &cminflt, &majflt, &cmajflt,
                   &utime, &stime, &cutime, &cstime,
                   &priority, &nice, &num_threads, &itrealvalue,
                   &starttime) != 20)
        {
            return p.cached;
        }

        proc_ticks = utime + stime + cutime + cstime; // include children
        start_time = starttime;
    }

    p.last_ms = now_ms;

    // --- Compute usage ---
    if (p.proc_ticks > 0 && p.start_time == start_time)
    {
        const double proc_delta = static_cast<double>(proc_ticks - p.proc_ticks);

        // Normalized CPU usage (max = 100%)
        const double current = (proc_delta / total_delta) * 100.0 / nproc;

        constexpr double alpha = 0.35;
        p.cached = p.cached * (1.0 - alpha) + current * alpha;
    }
    else
    {
        p.cached = 0.0;
    }

    p.proc_ticks = proc_ticks;
    p.start_time = start_time;

    return p.cached;
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

    static thread_local std::vector<uint8_t> buffer; // reused per thread to avoid repeated allocations
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
