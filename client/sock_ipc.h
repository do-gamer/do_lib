#ifndef SOCK_IPC_H
#define SOCK_IPC_H
#include <string>

// Unix domain socket connection to the browser's command server.
// Messages are newline delimited in both directions.
class SockIpc
{
public:
    SockIpc();
    ~SockIpc();

    bool Connected() const { return m_sock != -1; }

    // try to establish a connection to the unix domain socket at |path|.
    // if a previous socket exists it will be closed and recreated.
    bool Connect(const std::string &path);

    void Close();

    // send one message (a trailing newline is appended). On failure the
    // connection is closed so callers can reconnect.
    bool Send(const std::string &msg, int timeout_ms);

    // wait up to |timeout_ms| for the next complete line (without the newline).
    // returns false on timeout or when the connection breaks.
    bool RecvLine(std::string &line, int timeout_ms);

private:
    int m_sock = -1;
    std::string m_buffer;
};

#endif // SOCK_IPC_H
