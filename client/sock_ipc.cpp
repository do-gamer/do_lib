#include "sock_ipc.h"

#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
    using clock_type = std::chrono::steady_clock;

    int remaining_ms(clock_type::time_point deadline)
    {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_type::now()).count();
        return left > 0 ? static_cast<int>(left) : 0;
    }
}

SockIpc::SockIpc() = default;

SockIpc::~SockIpc()
{
    Close();
}

void SockIpc::Close()
{
    if (m_sock != -1)
    {
        close(m_sock);
        m_sock = -1;
    }
    m_buffer.clear();
}

bool SockIpc::Connect(const std::string &path)
{
    Close();

    sockaddr_un remote {};
    if (path.size() >= sizeof(remote.sun_path))
        return false;

    // non-blocking + close-on-exec: a hung browser can't stall us and the fd doesn't
    // leak into processes spawned by the JVM
    m_sock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_sock < 0)
    {
        m_sock = -1;
        return false;
    }

    remote.sun_family = AF_UNIX;
    std::memcpy(remote.sun_path, path.c_str(), path.size() + 1);

    // connect() on a unix socket completes immediately or fails (EAGAIN = backlog full)
    if (connect(m_sock, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) < 0)
    {
        Close();
        return false;
    }

    return true;
}

bool SockIpc::Send(const std::string &msg, int timeout_ms)
{
    if (m_sock == -1)
        return false;

    std::string data;
    data.reserve(msg.size() + 1);
    data.append(msg);
    data.push_back('\n');

    const auto deadline = clock_type::now() + std::chrono::milliseconds(timeout_ms);
    size_t written = 0;
    while (written < data.size())
    {
        // MSG_NOSIGNAL prevents SIGPIPE
        ssize_t n = send(m_sock, data.data() + written, data.size() - written, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0)
        {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n == -1 && errno == EINTR)
            continue;
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            int left = remaining_ms(deadline);
            pollfd pfd { m_sock, POLLOUT, 0 };
            if (left > 0 && poll(&pfd, 1, left) > 0 && !(pfd.revents & (POLLERR | POLLHUP)))
                continue;
        }

        Close();
        return false;
    }

    return true;
}

bool SockIpc::RecvLine(std::string &line, int timeout_ms)
{
    const auto deadline = clock_type::now() + std::chrono::milliseconds(timeout_ms);

    while (m_sock != -1)
    {
        size_t newline = m_buffer.find('\n');
        if (newline != std::string::npos)
        {
            line.assign(m_buffer, 0, newline);
            m_buffer.erase(0, newline + 1);
            return true;
        }

        char buf[4096];
        ssize_t n = recv(m_sock, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0)
        {
            m_buffer.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0)
        {
            Close(); // peer closed
            return false;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            Close();
            return false;
        }

        int left = remaining_ms(deadline);
        if (left <= 0)
            return false;

        pollfd pfd { m_sock, POLLIN, 0 };
        int r = poll(&pfd, 1, left);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
    }
    return false;
}
