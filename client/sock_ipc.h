#ifndef SOCK_IPC_H
#define SOCK_IPC_H
#include <string>



class SockIpc
{
public:
    SockIpc();
    ~SockIpc();

    bool Connected() const { return m_connected; }

    // try to establish a connection to the unix domain socket at |path|.
    // if a previous socket exists it will be closed and recreated.  returns
    // true on success, false otherwise.
    bool Connect(const std::string &path);

    // send a message over the socket. returns true on success; if the write
    // fails (broken pipe, connection reset, etc) the object will mark itself
    // disconnected so callers can attempt to reconnect.
    bool Send(const std::string &msg);

    // try to read a message from the socket. returns true if any data was read.
    // the call is non‑blocking and will mark the object disconnected on failure.
    bool Recv(std::string &msg);

    // internal state; public for legacy code but should not be touched
    bool m_connected = false;
    int m_sock = -1;
};

#endif // SOCK_IPC_H
