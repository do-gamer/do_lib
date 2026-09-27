#ifndef PROC_UTIL_H
#define PROC_UTIL_H

#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>

namespace ProcUtil
{
    struct MemPage
    {
        uintptr_t start, end;
        char read, write, exec, cow;
        std::string name;
    };

    bool IsChildOf(pid_t child_pid, pid_t test_parent);

    // All live descendants (children, grandchildren, ...) of |root|, single /proc pass.
    std::vector<pid_t> GetDescendants(pid_t root);

    // Command line with NUL separators replaced by spaces; empty if unavailable.
    std::string GetCmdline(pid_t pid);

    // True if the process is mapping a file whose path contains |name|.
    bool HasMapping(pid_t pid, const char *name);

    bool ProcessExists(pid_t pid);

    ssize_t ReadMemoryBytes(pid_t pid, uintptr_t address, void *dest, size_t size);
    ssize_t WriteMemoryBytes(pid_t pid, uintptr_t address, const void *src, size_t size);

    pid_t GetParent(pid_t pid);

    // Finds up to |amount| addresses of an exact byte pattern in readable memory of |pid|.
    std::vector<uintptr_t> QueryMemory(pid_t pid, const uint8_t *query, size_t size, size_t amount);

    std::vector<MemPage> GetPages(pid_t pid, const std::string &name = "");

    uint64_t GetMemoryUsage(pid_t pid);
    double GetCpuUsage(pid_t pid);
};

#endif /* PROC_UTIL_H */
