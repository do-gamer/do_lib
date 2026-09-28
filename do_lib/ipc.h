#ifndef IPC_H
#define IPC_H

#include <atomic>
#include <functional>
#include <thread>

#include "flash_ipc.h"

// Server side of the flash IPC (see tools/flash_ipc.h). Lives for the whole
// lifetime of the flash process: hook (un)installs only toggle the `installed`
// flag, so the runner thread never has to be joined from the game thread.
class Ipc
{
public:
    // Processes one request in place (the message becomes the response).
    // Only called while the game thread is alive (heartbeat is fresh).
    using Handler = std::function<flash_ipc::ResultCode(flash_ipc::Message &)>;

    Ipc() { }

    bool Init();

    bool Running() const { return m_running; }

    void Run(Handler handler);

    void Remove();

    // Called from the game thread on every timer tick.
    inline void Heartbeat()
    {
        if (auto *shared = m_shared.load(std::memory_order_acquire))
            shared->heartbeat_ms.store(flash_ipc::now_ms(), std::memory_order_release);
    }

    // ms since the game thread last ticked (large if never)
    inline uint64_t HeartbeatAgeMs() const
    {
        auto *shared = m_shared.load(std::memory_order_acquire);
        uint64_t hb = shared ? shared->heartbeat_ms.load(std::memory_order_acquire) : 0;
        return hb ? flash_ipc::now_ms() - hb : UINT64_MAX;
    }

    inline void SetInstalled(bool installed)
    {
        if (auto *shared = m_shared.load(std::memory_order_acquire))
            shared->installed.store(installed ? 1 : 0, std::memory_order_release);
    }

    ~Ipc();
private:
    void runner();

    Handler m_handler;

    std::thread m_runner_thread;
    int m_shmid = -1;
    // set once by Init(), never unmapped (see Remove)
    std::atomic<flash_ipc::Shared *> m_shared { nullptr };
    std::atomic<bool> m_running { false };
};


#endif /* IPC_H */
