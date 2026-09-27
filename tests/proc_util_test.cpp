// Tests client/proc_util.cpp against real child processes.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "proc_util.h"
#include "test_util.h"

using namespace std::chrono;

namespace
{
    const uint8_t PATTERN[8] = { 0xde, 0xad, 0xbe, 0xef, 0x13, 0x37, 0xc0, 0xde };

    struct Target
    {
        pid_t pid = -1;
        int report = -1; // child writes the addresses it planted

        std::vector<uintptr_t> planted;

        void start()
        {
            int fds[2];
            if (pipe(fds) != 0) abort();
            pid = fork();
            if (pid == 0)
            {
                close(fds[0]);

                // a huge reserved, unreadable region (like the flash/JIT reservations);
                // the old implementation tried to allocate a buffer of its full size
                mmap(nullptr, 64ull << 30, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

                // 40 MB readable region with patterns at chunk (8 MB) boundaries
                const size_t size = 40u << 20;
                auto *mem = static_cast<uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
                std::memset(mem, 0x11, size);
                const size_t offsets[] = { 100, (8u << 20) - 3, (16u << 20) - 7, (16u << 20) + 1, size - 8 };
                std::vector<uintptr_t> addrs;
                for (size_t off : offsets)
                {
                    std::memcpy(mem + off, PATTERN, sizeof(PATTERN));
                    addrs.push_back(reinterpret_cast<uintptr_t>(mem + off));
                }
                size_t n = addrs.size();
                if (write(fds[1], &n, sizeof(n)) < 0 || write(fds[1], addrs.data(), n * sizeof(uintptr_t)) < 0)
                    _exit(1);

                // grandchild for the descendant test
                if (fork() == 0)
                {
                    pause();
                    _exit(0);
                }
                pause();
                _exit(0);
            }
            close(fds[1]);
            size_t n = 0;
            if (read(fds[0], &n, sizeof(n)) != sizeof(n)) abort();
            planted.resize(n);
            if (read(fds[0], planted.data(), n * sizeof(uintptr_t)) != static_cast<ssize_t>(n * sizeof(uintptr_t))) abort();
            close(fds[0]);
            std::this_thread::sleep_for(milliseconds(50));
        }
    };

    long max_rss_kb()
    {
        rusage ru {};
        getrusage(RUSAGE_SELF, &ru);
        return ru.ru_maxrss;
    }
}

int main()
{
    test::Suite suite("proc util");

    Target target;
    target.start();

    suite.run("QueryMemory finds every match, including across chunk boundaries", [&]
    {
        long rss_before = max_rss_kb();
        auto t0 = steady_clock::now();
        auto found = ProcUtil::QueryMemory(target.pid, PATTERN, sizeof(PATTERN), 100);
        double ms = duration<double, std::milli>(steady_clock::now() - t0).count();

        // our own copy of PATTERN (in the test binary) doesn't exist in the child's anonymous
        // memory, but the child inherited this binary's data; keep only the planted region
        std::vector<uintptr_t> planted_found;
        for (auto a : found)
            if (std::find(target.planted.begin(), target.planted.end(), a) != target.planted.end())
                planted_found.push_back(a);

        CHECK_EQ(planted_found.size(), target.planted.size());
        test::info("scanned target with a 64 GB reservation in %.1f ms, peak RSS growth %ld KB", ms, max_rss_kb() - rss_before);
        CHECK(max_rss_kb() - rss_before < 64 * 1024);
    });

    suite.run("QueryMemory skips read-only file mappings (libraries), finds data in writable ones", [&]
    {
        // this binary's read-only data contains PATTERN (it's a constant); a writable copy doesn't
        auto found = ProcUtil::QueryMemory(getpid(), PATTERN, sizeof(PATTERN), 100);
        bool in_ro_file = false;
        for (auto &page : ProcUtil::GetPages(getpid()))
            for (auto a : found)
                if (a >= page.start && a < page.end && page.write != 'w' && !page.name.empty() && page.name[0] == '/')
                    in_ro_file = true;
        CHECK(!in_ro_file);

        static uint8_t writable_copy[sizeof(PATTERN)];
        std::memcpy(writable_copy, PATTERN, sizeof(PATTERN));
        found = ProcUtil::QueryMemory(getpid(), PATTERN, sizeof(PATTERN), 100);
        CHECK(std::find(found.begin(), found.end(), reinterpret_cast<uintptr_t>(writable_copy)) != found.end());
    });

    suite.run("QueryMemory respects the result limit", [&]
    {
        CHECK_EQ(ProcUtil::QueryMemory(target.pid, PATTERN, sizeof(PATTERN), 2).size(), 2u);
    });

    suite.run("QueryMemory handles bad input", [&]
    {
        CHECK(ProcUtil::QueryMemory(target.pid, PATTERN, 0, 10).empty());
        CHECK(ProcUtil::QueryMemory(-1, PATTERN, sizeof(PATTERN), 10).empty());
        CHECK(ProcUtil::QueryMemory(target.pid, nullptr, 4, 10).empty());
    });

    suite.run("ReadMemoryBytes / WriteMemoryBytes", [&]
    {
        uint8_t buf[8] = {};
        CHECK_EQ(ProcUtil::ReadMemoryBytes(target.pid, target.planted[0], buf, 8), 8);
        CHECK(std::memcmp(buf, PATTERN, 8) == 0);
        uint32_t value = 0x12345678;
        CHECK_EQ(ProcUtil::WriteMemoryBytes(target.pid, target.planted[0], &value, 4), 4);
        uint32_t back = 0;
        ProcUtil::ReadMemoryBytes(target.pid, target.planted[0], &back, 4);
        CHECK_EQ(back, value);
        CHECK(ProcUtil::ReadMemoryBytes(target.pid, 0x10, buf, 8) < 0);
    });

    suite.run("GetPages parses maps with names", [&]
    {
        auto pages = ProcUtil::GetPages(getpid());
        CHECK(!pages.empty());
        bool has_self = false, has_stack = false;
        for (auto &p : pages)
        {
            has_self |= p.name.find("proc_util_test") != std::string::npos;
            has_stack |= p.name == "[stack]";
        }
        CHECK(has_self);
        CHECK(has_stack);
        CHECK(!ProcUtil::GetPages(getpid(), "libc").empty());
        CHECK(ProcUtil::HasMapping(getpid(), "proc_util_test"));
        CHECK(!ProcUtil::HasMapping(getpid(), "libpepflashplayer"));
    });

    suite.run("GetDescendants / IsChildOf / GetParent", [&]
    {
        auto desc = ProcUtil::GetDescendants(getpid());
        CHECK(std::find(desc.begin(), desc.end(), target.pid) != desc.end());
        CHECK(desc.size() >= 2); // child + grandchild
        CHECK(ProcUtil::IsChildOf(target.pid, getpid()));
        CHECK_EQ(ProcUtil::GetParent(target.pid), getpid());
        CHECK(!ProcUtil::IsChildOf(getpid(), target.pid));

        auto t0 = steady_clock::now();
        for (int i = 0; i < 20; i++)
            ProcUtil::GetDescendants(getpid());
        test::info("GetDescendants: %.2f ms per full /proc scan", duration<double, std::milli>(steady_clock::now() - t0).count() / 20);
    });

    suite.run("GetCmdline", [&]
    {
        CHECK(ProcUtil::GetCmdline(getpid()).find("proc_util_test") != std::string::npos);
        CHECK(ProcUtil::GetCmdline(999999999).empty());
    });

    suite.run("ProcessExists / memory / cpu usage", [&]
    {
        CHECK(ProcUtil::ProcessExists(getpid()));
        CHECK(!ProcUtil::ProcessExists(-1));
        CHECK(ProcUtil::GetMemoryUsage(getpid()) > 0);
        ProcUtil::GetCpuUsage(getpid());
        auto until = steady_clock::now() + milliseconds(300);
        volatile uint64_t spin = 0;
        while (steady_clock::now() < until) spin++;
        double cpu = ProcUtil::GetCpuUsage(getpid());
        test::info("cpu usage while spinning: %.1f%%", cpu);
        CHECK(cpu > 0);
    });

    suite.run("dead process is detected", [&]
    {
        auto desc = ProcUtil::GetDescendants(target.pid);
        kill(target.pid, SIGKILL);
        for (pid_t p : desc) kill(p, SIGKILL);
        waitpid(target.pid, nullptr, 0);
        CHECK(!ProcUtil::ProcessExists(target.pid));
        CHECK(ProcUtil::QueryMemory(target.pid, PATTERN, sizeof(PATTERN), 10).empty());
    });

    return suite.finish();
}
