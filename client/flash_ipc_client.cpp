#include "flash_ipc_client.h"

#include <atomic>
#include <cstring>

#include <sys/ipc.h>
#include <sys/shm.h>

#include "utils.h"

using flash_ipc::Message;
using flash_ipc::ResultCode;
using flash_ipc::now_ms;

namespace
{
    // Logs at most once per |interval_ms| for a given call site.
    class RateLimit
    {
    public:
        explicit RateLimit(uint64_t interval_ms) : m_interval(interval_ms) { }
        bool Allow()
        {
            uint64_t now = now_ms();
            uint64_t last = m_last.load(std::memory_order_relaxed);
            if (last && now - last < m_interval)
                return false;
            return m_last.compare_exchange_strong(last, now);
        }
    private:
        uint64_t m_interval;
        std::atomic<uint64_t> m_last { 0 };
    };

    bool wait_response(flash_ipc::Shared *shared, uint32_t seq, int timeout_ms)
    {
        const uint64_t deadline = now_ms() + static_cast<uint64_t>(timeout_ms);
        while (true)
        {
            uint32_t resp = shared->resp_seq.load(std::memory_order_acquire);
            if (resp == seq)
                return true;

            uint64_t now = now_ms();
            if (now >= deadline)
                return false;
            flash_ipc::futex_wait(&shared->resp_seq, resp, static_cast<int>(deadline - now));
        }
    }
}

FlashIpcClient::~FlashIpcClient()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    detach(false);
}

void FlashIpcClient::detach(bool remove)
{
    if (m_shared)
        shmdt(m_shared);

    if (remove && m_shmid >= 0)
        shmctl(m_shmid, IPC_RMID, nullptr);

    m_shared = nullptr;
    m_shmid = -1;
    m_pid = -1;
    m_heartbeat_seen = 0;
    m_heartbeat_changed_ms = 0;
    m_frozen_logged = false;
}

bool FlashIpcClient::attach(int pid)
{
    if (pid <= 0)
        return false;

    if (m_shared)
    {
        if (m_pid == pid && m_shared->magic.load(std::memory_order_acquire) == flash_ipc::MAGIC)
            return true;
        // a different process: its segment is garbage now; same process: server went away
        detach(m_pid != pid);
    }

    // Created by do_lib when it installs; never create it from this side.
    int shmid = shmget(pid, 0, 0);
    if (shmid < 0)
        return false;

    void *mem = shmat(shmid, nullptr, 0);
    if (mem == reinterpret_cast<void *>(-1))
        return false;

    auto *shared = static_cast<flash_ipc::Shared *>(mem);
    if (shared->magic.load(std::memory_order_acquire) != flash_ipc::MAGIC)
    {
        shmdt(mem);
        return false;
    }

    m_shared = shared;
    m_shmid = shmid;
    m_pid = pid;
    utils::log("[FlashIpc] attached (pid {})\n", pid);
    return true;
}

void FlashIpcClient::Reset(int pid, bool remove)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    bool had_segment = m_shmid >= 0 && m_pid == pid;
    detach(remove);

    // do_lib may have created the segment before we ever attached
    if (remove && !had_segment && pid > 0)
    {
        int shmid = shmget(pid, 0, 0);
        if (shmid >= 0)
            shmctl(shmid, IPC_RMID, nullptr);
    }
}

bool FlashIpcClient::IsFrozen(int pid, uint64_t frozen_ms)
{
    // Called from the bot's main tick: never wait behind a call another thread has in
    // flight (up to the sync timeout); a call in progress means the game is being talked to.
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false;
    if (!attach(pid))
        return false; // not installed yet, nothing to judge

    const uint64_t heartbeat = m_shared->heartbeat_ms.load(std::memory_order_acquire);
    const uint64_t now = now_ms();

    if (heartbeat != m_heartbeat_seen)
    {
        m_heartbeat_seen = heartbeat;
        m_heartbeat_changed_ms = now;
        m_frozen_logged = false;
        return false;
    }

    if (!heartbeat || now - m_heartbeat_changed_ms < frozen_ms)
        return false;

    if (!m_frozen_logged)
    {
        m_frozen_logged = true;
        utils::log("[FlashIpc] game thread not responding for {}s (hooks installed: {})\n",
                   (now - m_heartbeat_changed_ms) / 1000,
                   m_shared->installed.load() != 0);
    }
    return true;
}

bool FlashIpcClient::Send(int pid, const Message &message, Message *response)
{
    static RateLimit not_ready_log(10'000);
    static RateLimit error_log(2'000);

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!attach(pid))
    {
        if (not_ready_log.Allow())
            utils::log("[FlashIpc] not available yet\n");
        return false;
    }

    flash_ipc::Shared *shared = m_shared;

    // fail fast instead of blocking the caller when the game can't execute anything; async
    // commands need a fresher heartbeat since the server drops them after ASYNC_MAX_DELAY_MS
    if (!flash_ipc::game_thread_alive(shared, flash_ipc::is_async(message.type)
            ? flash_ipc::ASYNC_MAX_DELAY_MS : flash_ipc::STALL_MS))
    {
        if (not_ready_log.Allow())
            utils::log("[FlashIpc] game thread not ready\n");
        return false;
    }

    uint32_t seq = shared->req_seq.load(std::memory_order_acquire);

    // A previous request timed out on our side but the server may still be working on it.
    // Never overwrite the slot while it's busy.
    if (shared->resp_seq.load(std::memory_order_acquire) != seq && !wait_response(shared, seq, 100))
    {
        if (error_log.Allow())
            utils::log("[FlashIpc] busy with a previous request\n");
        return false;
    }

    std::memcpy(static_cast<void *>(&shared->msg), static_cast<const void *>(&message), sizeof(Message));
    ++seq;
    shared->req_seq.store(seq, std::memory_order_release);
    flash_ipc::futex_wake(&shared->req_seq);

    const int timeout = flash_ipc::is_async(message.type)
        ? flash_ipc::CLIENT_ASYNC_TIMEOUT_MS
        : flash_ipc::CLIENT_SYNC_TIMEOUT_MS;

    if (!wait_response(shared, seq, timeout))
    {
        if (error_log.Allow())
            utils::log("[FlashIpc] timeout waiting for flash (type {})\n", static_cast<int>(message.type));
        // An async command is published and will most likely still be executed. Reporting a
        // failure would make the bot fall back to e.g. the slot keybind and use an item twice.
        return flash_ipc::is_async(message.type);
    }

    auto status = static_cast<ResultCode>(shared->status.load(std::memory_order_relaxed));

    if (response)
        std::memcpy(static_cast<void *>(response), static_cast<const void *>(&shared->msg), sizeof(Message));

    if (status != ResultCode::OK)
    {
        if (error_log.Allow())
            utils::log("[FlashIpc] status {} (type {})\n", static_cast<int>(status), static_cast<int>(message.type));
        return false;
    }

    return true;
}
