#include "sock_ipc.h"

#include <stdexcept>

#include <cstdio>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>    // for fcntl, O_NONBLOCK

SockIpc::SockIpc()
{
    m_sock = -1;
}

SockIpc::~SockIpc() 
{
    if (m_sock != -1)
    {
        close(m_sock);
    }
}

static int create_unix_socket()
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0)
        throw std::runtime_error { "Failed to create ipc socket" };
    return s;
}

bool SockIpc::Connect(const std::string &path)
{
    // if we already have a socket, close it before recreating
    if (m_sock != -1)
    {
        close(m_sock);
        m_sock = -1;
        m_connected = false;
    }

    m_sock = create_unix_socket();

    sockaddr_un m_remote;
    m_remote.sun_family = AF_UNIX;
    strncpy(m_remote.sun_path, path.c_str(), sizeof(m_remote.sun_path));
    m_remote.sun_path[sizeof(m_remote.sun_path) - 1] = '\0';

    if (connect(m_sock, reinterpret_cast<sockaddr *>(&m_remote), sizeof(m_remote)) < 0)
    {
        close(m_sock);
        m_sock = -1;
        return false;
    }

    // operate the socket in non-blocking mode so that a hung browser cannot
    // cause our entire process to stall when we try to write to the socket.
    int flags = fcntl(m_sock, F_GETFL, 0);
    if (flags != -1)
        fcntl(m_sock, F_SETFL, flags | O_NONBLOCK);

    m_connected = true;
    return true;
}

bool SockIpc::Send(const std::string &msg)
{
    if (!m_connected || m_sock == -1)
        return false;

    const char *buf = msg.data();
    size_t to_write = msg.size();
    size_t written = 0;
    while (written < to_write)
    {
        // MSG_NOSIGNAL prevents SIGPIPE, MSG_DONTWAIT respects the
        // non-blocking flag so we return immediately if the buffer is full.
        ssize_t n = send(m_sock, buf + written, to_write - written,
                         MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0)
        {
            written += static_cast<size_t>(n);
            continue;
        }
        if (n == -1 && errno == EINTR)
            continue;

        // an error occurred, mark ourselves disconnected and bail out
        m_connected = false;
        close(m_sock);
        m_sock = -1;
        return false;
    }

    return true;
}

// non-blocking receive; returns true if data was read into msg.
bool SockIpc::Recv(std::string &msg)
{
    if (!m_connected || m_sock == -1)
        return false;

    char buf[1024];
    ssize_t n = recv(m_sock, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    if (n <= 0)
    {
        if (n == 0 || (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK))
        {
            m_connected = false;
            close(m_sock);
            m_sock = -1;
        }
        return false;
    }

    buf[n] = '\0';
    msg.assign(buf, static_cast<size_t>(n));
    return true;
}

