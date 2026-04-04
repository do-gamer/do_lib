#ifndef TOOLS_UTILS_H
#define TOOLS_UTILS_H

#include <cmath>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>

namespace fs = std::filesystem;

namespace utils
{
    inline const std::string &log_timestamp_str()
    {
        static const std::string ts = []() -> std::string {
            if (const char *env = getenv("TANOS_START_TIME"))
                return env;

            std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_r(&t, &tm);
            std::stringstream ss;
            ss << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
            const std::string s = ss.str();
            setenv("TANOS_START_TIME", s.c_str(), 0);
            return s;
        }();
        return ts;
    }

    inline const std::string &get_log_file_path()
    {
        static const std::string path = []()
        {
            const std::string &ts = log_timestamp_str();
            if (ts.empty())
                return std::string{};

            std::string filename = ts + "_TanosApi.log";
            fs::path log_dir = "logs";
            std::error_code ec;
            fs::create_directories(log_dir, ec);
            if (ec)
                return filename;

            return (log_dir / filename).string();
        }();

        return path;
    }

    static inline void format(std::stringstream &of, const char *data)
    {
        of << data;
    }

    static inline std::string format(const char *data)
    {
        std::stringstream ss;
        format(ss, data);
        return ss.str();
    }

    template <typename T, typename... Args>
    static void format(std::stringstream &of, const char *s, T value, Args... args)
    {
        const char *start = s;
        for (; *s != 0; s++)
        {
            if (*s == '{' && (s == start || *(s - 1) != '\\'))
            {
                char key = '\x00';
                for (s++; *s != 0; s++)
                {
                    if (*s == ' ')
                        continue;
                    else if (*s == '}' && *(s - 1) != '\\')
                    {
                        if (key == 'x')
                            of << std::hex;
                        else
                            of << std::dec;
                        of << value;
                        format(of, s + 1, args...);
                        return;
                    }
                    else if (key)
                    {
                        key = '\x00';
                        break;
                    }
                    else
                        key = *s;
                }
            }
            of << *s;
        }
    }

    template <typename T, typename... Args>
    static inline std::string format(const char *s, T value, Args... args)
    {
        std::stringstream ss;
        format(ss, s, value, args...);
        return ss.str();
    }

    template <typename T, typename... Args>
    static inline std::string format(const std::string &s, T value, Args... args)
    {
        return format(s.c_str(), value, args...);
    }

    static inline void log(const char *data)
    {
        const std::string &path = get_log_file_path();
        if (path.empty()) return;
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_r(&t, &tm);
        std::ofstream fhandle{path, std::ios::app};
        fhandle << std::put_time(&tm, "[%Y-%m-%d %H:%M:%S] ") << data;
    }

    template <typename T, typename... Args>
    static inline void log(const char *s, T value, Args... args)
    {
        std::string formatted = format(s, value, args...);
        log(formatted.c_str());
    }

    static inline std::string escape_json(const std::string& s)
    {
        std::string res = "\"";
        for (char c : s) {
            if (c == '"') res += "\\\"";
            else if (c == '\\') res += "\\\\";
            else if (c == '\n') res += "\\n";
            else if (c == '\r') res += "\\r";
            else if (c == '\t') res += "\\t";
            else if (c == '\b') res += "\\b";
            else if (c == '\f') res += "\\f";
            else res += c;
        }
        res += "\"";
        return res;
    }

    class vec2
    {
    public:
        vec2() : x(0), y(0) { }
        vec2(int n) : x(n), y(n) { }
        vec2(int x, int y) : x(x), y(y) { }
        vec2(float n) : x(n), y(n) { }
        vec2(float x, float y) : x(x), y(y) { }
        vec2(double x, double y) : x(x), y(y) { }
        vec2(const vec2 &p) : x(p.x), y(p.y) { }

        float distance(float x, float y) const
        {
            return sqrt(pow(this->x - x, 2) + pow(this->y - y, 2));
        }

        float distance(int x, int y) const
        {
            return distance(static_cast<float>(x), static_cast<float>(y));
        }

        float distance(const vec2 &other) const
        {
            return distance(other.x, other.y);
        }

        vec2 MapTo(float mx, float my) const
        {
            return vec2(x * mx, y * my);
        }

        vec2 operator+=(const vec2 &rhs)
        {
            x += rhs.x;
            y += rhs.y;
            return *this;
        }
        vec2 operator+=(const float &rhs)
        {
            x += rhs;
            y += rhs;
            return *this;
        }
        vec2 operator+=(const int &rhs) { return *this += static_cast<float>(rhs); }

        vec2 operator-=(const vec2 &rhs)
        {
            x -= rhs.x;
            y -= rhs.y;
            return *this;
        }
        vec2 operator-=(const float &rhs)
        {
            x -= rhs;
            y -= rhs;
            return *this;
        }
        vec2 operator-=(const int &rhs) { return *this -= static_cast<float>(rhs); }

        vec2 operator*=(const vec2 &rhs)
        {
            x *= rhs.x;
            y *= rhs.y;
            return *this;
        }
        vec2 operator*=(const float &rhs)
        {
            x *= rhs;
            y *= rhs;
            return *this;
        }
        vec2 operator*=(const int &rhs) { return *this *= static_cast<float>(rhs); }

        vec2 operator/=(const vec2 &rhs)
        {
            x /= rhs.x;
            y /= rhs.y;
            return *this;
        }
        vec2 operator/=(const float &rhs)
        {
            x /= rhs;
            y /= rhs;
            return *this;
        }
        vec2 operator/=(const int &rhs) { return *this /= static_cast<float>(rhs); }

        friend vec2 operator+(vec2 lhs, const vec2 &rhs)
        {
            lhs += rhs;
            return lhs;
        }

        friend vec2 operator+(vec2 lhs, const float &rhs)
        {
            lhs += rhs;
            return lhs;
        }

        friend vec2 operator+(vec2 lhs, const int &rhs) { return lhs + static_cast<float>(rhs); }

        friend vec2 operator-(vec2 lhs, const vec2 &rhs)
        {
            lhs -= rhs;
            return lhs;
        }

        friend vec2 operator-(vec2 lhs, const float &rhs)
        {
            lhs -= rhs;
            return lhs;
        }

        friend vec2 operator-(vec2 lhs, const int &rhs) { return lhs - static_cast<float>(rhs); }

        friend vec2 operator*(vec2 lhs, const vec2 &rhs)
        {
            lhs *= rhs;
            return lhs;
        }

        friend vec2 operator*(vec2 lhs, const float &rhs)
        {
            lhs *= rhs;
            return lhs;
        }

        friend vec2 operator*(vec2 lhs, const int &rhs) { return lhs * static_cast<float>(rhs); }

        friend vec2 operator/(vec2 lhs, const vec2 &rhs)
        {
            lhs /= rhs;
            return lhs;
        }

        friend vec2 operator/(vec2 lhs, const float &rhs)
        {
            lhs /= rhs;
            return lhs;
        }

        friend vec2 operator/(vec2 lhs, const int &rhs) { return lhs / static_cast<float>(rhs); }

        bool operator==(const vec2 &other) const
        {
            return(x == other.x && y == other.y);
        }

        bool operator!=(const vec2 &other)
        {
            return !(*this == other);
        }

        vec2 &operator=(const vec2 &other)
        {
            if (this != &other) {
                x = other.x;
                y = other.y;
            }
            return *this;
        }

        friend std::ostream& operator<<(std::ostream& os, const utils::vec2& vec)
        {
            os << std::fixed << "vec2(" << vec.x << ", " << vec.y << ") ";
            return os;
        }

        float x, y;
    };

};

#endif /* TOOLS_UTILS_H */