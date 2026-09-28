#include "proc_util.h"
#include <algorithm>
#include <unordered_map>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <sys/uio.h>
#include <unistd.h>

namespace
{
    // Reads a small /proc file into buf (NUL terminated). Returns bytes read or -1.
    ssize_t read_proc_file(const char *path, char *buf, size_t size)
    {
        int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return -1;

        size_t total = 0;
        while (total < size - 1)
        {
            ssize_t n = ::read(fd, buf + total, size - 1 - total);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            total += static_cast<size_t>(n);
        }
        ::close(fd);
        buf[total] = '\0';
        return static_cast<ssize_t>(total);
    }

    // Parses the fields after "comm" of /proc/<pid>/stat. comm may contain spaces
    // and parentheses, so scan from the last ')'.
    const char *stat_fields(pid_t pid, char *buf, size_t size)
    {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        if (read_proc_file(path, buf, size) <= 0)
            return nullptr;

        const char *rp = strrchr(buf, ')');
        if (!rp || rp[1] == '\0' || rp[2] == '\0')
            return nullptr;
        return rp + 2;
    }

    struct StatInfo
    {
        char state = 0;
        pid_t ppid = 0;
        unsigned long utime = 0, stime = 0;
        long cutime = 0, cstime = 0;
        unsigned long long starttime = 0;
        long rss = 0;
    };

    bool read_stat(pid_t pid, StatInfo &out)
    {
        char buf[1024];
        const char *fields = stat_fields(pid, buf, sizeof(buf));
        if (!fields)
            return false;

        int pgrp, session, tty_nr, tpgid;
        unsigned int flags;
        unsigned long minflt, cminflt, majflt, cmajflt;
        long priority, nice, num_threads, itrealvalue;
        unsigned long vsize;

        int n = sscanf(fields,
                       "%c %d %d %d %d %d %u "
                       "%lu %lu %lu %lu %lu %lu %ld %ld "
                       "%ld %ld %ld %ld %llu "
                       "%lu %ld",
                       &out.state, &out.ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
                       &minflt, &cminflt, &majflt, &cmajflt, &out.utime, &out.stime,
                       &out.cutime, &out.cstime,
                       &priority, &nice, &num_threads, &itrealvalue,
                       &out.starttime,
                       &vsize, &out.rss);
        return n == 22;
    }

    template <typename F>
    void for_each_pid(F &&f)
    {
        DIR *dir = opendir("/proc");
        if (!dir)
            return;

        while (dirent *entry = readdir(dir))
        {
            const char *name = entry->d_name;
            if (*name < '1' || *name > '9')
                continue;

            char *end = nullptr;
            long pid = strtol(name, &end, 10);
            if (*end == '\0' && pid > 0)
                f(static_cast<pid_t>(pid));
        }
        closedir(dir);
    }
}

bool ProcUtil::IsChildOf(pid_t child_pid, pid_t test_parent)
{
    if (test_parent <= 0)
        return false;

    pid_t pid = child_pid;
    // walk up the parent chain but avoid an infinite loop; limit depth
    for (int depth = 0; depth < 128; ++depth)
    {
        pid_t parent = GetParent(pid);
        if (parent == test_parent)
            return true;
        if (parent <= 1 || parent == pid)
            break;
        pid = parent;
    }
    return false;
}

std::vector<pid_t> ProcUtil::GetDescendants(pid_t root)
{
    std::vector<pid_t> result;
    if (root <= 0)
        return result;

    std::unordered_multimap<pid_t, pid_t> children;
    for_each_pid([&](pid_t pid)
    {
        pid_t parent = GetParent(pid);
        if (parent > 0)
            children.emplace(parent, pid);
    });

    std::vector<pid_t> stack { root };
    while (!stack.empty() && result.size() < 4096)
    {
        pid_t current = stack.back();
        stack.pop_back();

        auto range = children.equal_range(current);
        for (auto it = range.first; it != range.second; ++it)
        {
            result.push_back(it->second);
            stack.push_back(it->second);
        }
    }
    return result;
}

std::string ProcUtil::GetCmdline(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

    char buf[8192];
    ssize_t n = read_proc_file(path, buf, sizeof(buf));
    if (n <= 0)
        return {};

    std::string contents(buf, static_cast<size_t>(n));
    std::replace(contents.begin(), contents.end(), '\0', ' ');
    return contents;
}

bool ProcUtil::HasMapping(pid_t pid, const char *name)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    FILE *f = fopen(path, "re");
    if (!f)
        return false;

    char line[4096];
    bool found = false;
    while (fgets(line, sizeof(line), f))
    {
        if (strstr(line, name))
        {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

bool ProcUtil::ProcessExists(pid_t pid)
{
    if (pid <= 0)
        return false;
    return kill(pid, 0) == 0 || errno == EPERM;
}

ssize_t ProcUtil::ReadMemoryBytes(pid_t pid, uintptr_t address, void *dest, size_t size)
{
    iovec local_addr { dest, size };
    iovec remote_addr { reinterpret_cast<void *>(address), size };

    return process_vm_readv(pid, &local_addr, 1, &remote_addr, 1, 0);
}

ssize_t ProcUtil::WriteMemoryBytes(pid_t pid, uintptr_t address, const void *src, size_t size)
{
    iovec local_addr { const_cast<void *>(src), size };
    iovec remote_addr { reinterpret_cast<void *>(address), size };
    return process_vm_writev(pid, &local_addr, 1, &remote_addr, 1, 0);
}

std::vector<ProcUtil::MemPage> ProcUtil::GetPages(pid_t pid, const std::string &name)
{
    std::vector<MemPage> pages;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    FILE *f = fopen(path, "re");
    if (!f)
        return pages;

    char line[4096];
    while (fgets(line, sizeof(line), f))
    {
        unsigned long start, end;
        char perms[5] = {};
        int name_pos = 0;

        // start-end perms offset dev inode [pathname]
        if (sscanf(line, "%lx-%lx %4s %*x %*x:%*x %*u %n", &start, &end, perms, &name_pos) < 3)
            continue;

        std::string filename;
        if (name_pos > 0)
        {
            filename = line + name_pos;
            while (!filename.empty() && (filename.back() == '\n' || filename.back() == ' '))
                filename.pop_back();
        }

        if (!name.empty() && filename.find(name) == std::string::npos)
            continue;

        pages.push_back(MemPage { start, end, perms[0], perms[1], perms[2], perms[3], std::move(filename) });
    }
    fclose(f);
    return pages;
}

uint64_t ProcUtil::GetMemoryUsage(pid_t pid)
{
    StatInfo st;
    if (pid <= 0 || !read_stat(pid, st))
        return 0;

    static const long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024;
    return static_cast<uint64_t>(st.rss) * static_cast<uint64_t>(page_size_kb);
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

    if (pid <= 0)
        return 0.0;

    // pids change on every refresh; don't let the map grow forever
    if (prev.size() > 16 && prev.find(pid) == prev.end())
        prev.clear();

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
    StatInfo st;
    if (!read_stat(pid, st))
        return p.cached;

    const uint64_t proc_ticks = st.utime + st.stime + st.cutime + st.cstime; // include children
    const uint64_t start_time = st.starttime;

    p.last_ms = now_ms;

    // --- Compute usage ---
    if (p.proc_ticks > 0 && p.start_time == start_time && proc_ticks >= p.proc_ticks)
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

std::vector<uintptr_t> ProcUtil::QueryMemory(pid_t pid, const uint8_t *query, size_t size, size_t amount)
{
    std::vector<uintptr_t> result;
    if (!query || size == 0 || amount == 0 || pid <= 0)
        return result;

    // Read regions in bounded chunks: reserved/huge mappings never cause big allocations,
    // chunks overlap by size-1 bytes so matches crossing a boundary are still found.
    // 1 MB keeps each chunk in L2 cache while it's searched (~20% faster than 8 MB).
    constexpr size_t CHUNK = 1u << 20;
    // No real data region is this large; skips e.g. sanitizer shadow maps (terabytes).
    constexpr size_t MAX_REGION = 16ull << 30;
    static thread_local std::vector<uint8_t> buffer;
    if (buffer.size() < CHUNK)
        buffer.resize(CHUNK);

    for (const auto &region : GetPages(pid))
    {
        if (region.read != 'r' || region.end <= region.start
            || region.name == "[vvar]" || region.name == "[vsyscall]" || region.name == "[vvar_vclock]")
            continue;

        // Read-only file mappings (browser binary, libraries, fonts) are ~60% of a flash
        // process' readable memory and never contain game objects; the heap is anonymous.
        if (region.write != 'w' && !region.name.empty() && region.name[0] == '/')
            continue;

        const size_t region_size = region.end - region.start;
        if (size > region_size || region_size > MAX_REGION)
            continue;

        size_t offset = 0;
        while (offset + size <= region_size)
        {
            const size_t want = std::min(CHUNK, region_size - offset);
            const ssize_t got = ReadMemoryBytes(pid, region.start + offset, buffer.data(), want);
            if (got < static_cast<ssize_t>(size))
                break; // region vanished or unreadable

            const size_t len = static_cast<size_t>(got);
            const uint8_t *hay = buffer.data();
            const uint8_t *pos = hay;
            size_t remaining = len;

            while (remaining >= size)
            {
                const void *found = memmem(pos, remaining, query, size);
                if (!found)
                    break;

                const size_t found_off = static_cast<const uint8_t *>(found) - hay;
                result.push_back(region.start + offset + found_off);
                if (result.size() >= amount)
                    return result;

                pos = static_cast<const uint8_t *>(found) + 1;
                remaining = len - (found_off + 1);
            }

            if (len < want || offset + len >= region_size)
                break;
            offset += len - (size - 1);
        }
    }

    return result;
}

pid_t ProcUtil::GetParent(pid_t pid)
{
    char buf[512];
    const char *fields = stat_fields(pid, buf, sizeof(buf));
    if (!fields)
        return 0;

    char state;
    int parent = 0;
    if (sscanf(fields, "%c %d", &state, &parent) != 2)
        return 0;
    return parent;
}
