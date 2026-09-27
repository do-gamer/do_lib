#ifndef TEST_UTIL_H
#define TEST_UTIL_H

// Minimal test helpers (no external dependencies).
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>

namespace test
{
    inline int &failures() { static int f = 0; return f; }

    inline void info(const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        std::printf("      ");
        std::vprintf(fmt, args);
        std::printf("\n");
        va_end(args);
    }

    class Suite
    {
    public:
        explicit Suite(const char *name) : m_name(name) { std::printf("== %s\n", name); }

        void run(const char *name, const std::function<void()> &body)
        {
            int before = failures();
            body();
            bool ok = failures() == before;
            std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
            std::fflush(stdout);
            ok ? m_passed++ : m_failed++;
        }

        int finish()
        {
            std::printf("== %s: %d passed, %d failed\n", m_name.c_str(), m_passed, m_failed);
            return m_failed ? 1 : 0;
        }

    private:
        std::string m_name;
        int m_passed = 0, m_failed = 0;
    };
}

#define CHECK(...) do { if (!(__VA_ARGS__)) { std::printf("      check failed: %s (%s:%d)\n", #__VA_ARGS__, __FILE__, __LINE__); test::failures()++; } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) { std::printf("      check failed: %s == %s (%lld vs %lld) (%s:%d)\n", #a, #b, (long long)_a, (long long)_b, __FILE__, __LINE__); test::failures()++; } } while (0)

#endif // TEST_UTIL_H
