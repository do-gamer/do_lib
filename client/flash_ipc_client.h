#ifndef FLASH_IPC_CLIENT_H
#define FLASH_IPC_CLIENT_H

#include <cstdint>
#include <mutex>

#include "flash_ipc.h"

// Client side of the flash IPC (see tools/flash_ipc.h). Thread-safe.
class FlashIpcClient
{
public:
    ~FlashIpcClient();

    // Sends a request to the do_lib instance inside process |pid|. Returns true when the
    // server executed (or, for async commands, accepted) it; copies the response if given.
    bool Send(int pid, const flash_ipc::Message &message, flash_ipc::Message *response = nullptr);

    // True when the game thread of |pid| hasn't ticked for |frozen_ms| while connected.
    bool IsFrozen(int pid, uint64_t frozen_ms);

    // Forgets the connection. With remove, the segment of |pid| is deleted too: do_lib
    // can't clean up when flash is SIGKILLed on refresh, and leaked segments eventually
    // exhaust the system limit (shmmni).
    void Reset(int pid, bool remove);

private:
    bool attach(int pid);
    void detach(bool remove);

    std::mutex m_mutex;
    flash_ipc::Shared *m_shared = nullptr;
    int m_shmid = -1;
    int m_pid = -1;                 // flash pid the segment belongs to
    uint64_t m_heartbeat_seen = 0;  // last heartbeat value observed
    uint64_t m_heartbeat_changed_ms = 0;
    bool m_frozen_logged = false;
};

#endif // FLASH_IPC_CLIENT_H
