#include "ipc.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include <unistd.h>
#include <sys/shm.h>
#include <sys/ipc.h>

#include "utils.h"

using flash_ipc::Message;
using flash_ipc::ResultCode;

bool Ipc::Init()
{
    if (m_shared.load())
        return true;

    key_t key = getpid();

    // A segment with our key may be left over from a killed process that had the same pid.
    m_shmid = shmget(key, flash_ipc::SHM_SIZE, IPC_CREAT | IPC_EXCL | 0600);
    if (m_shmid < 0 && errno == EEXIST)
    {
        int stale = shmget(key, 0, 0);
        if (stale >= 0)
        {
            // invalidate it first so a client still attached to it notices and re-attaches
            void *old = shmat(stale, nullptr, 0);
            if (old != reinterpret_cast<void *>(-1))
            {
                static_cast<flash_ipc::Shared *>(old)->magic.store(0);
                shmdt(old);
            }
            shmctl(stale, IPC_RMID, nullptr);
        }
        m_shmid = shmget(key, flash_ipc::SHM_SIZE, IPC_CREAT | IPC_EXCL | 0600);
    }

    if (m_shmid < 0)
    {
        utils::log("[Ipc::init] Failed to get shared memory: {}\n", strerror(errno));
        return false;
    }

    void *mem = shmat(m_shmid, nullptr, 0);
    if (mem == reinterpret_cast<void *>(-1))
    {
        utils::log("[Ipc::init] Failed to attach shared memory to our process: {}\n", strerror(errno));
        shmctl(m_shmid, IPC_RMID, nullptr);
        m_shmid = -1;
        return false;
    }

    std::memset(mem, 0, flash_ipc::SHM_SIZE);
    auto *shared = new (mem) flash_ipc::Shared();
    shared->magic.store(flash_ipc::MAGIC, std::memory_order_release);
    m_shared = shared;

    return true;
}

void Ipc::Run(Handler handler)
{
    if (m_running || !m_shared.load())
        return;

    m_handler = std::move(handler);
    m_running = true;
    m_runner_thread = std::thread(&Ipc::runner, this);
}

void Ipc::Remove()
{
    if (m_running)
    {
        m_running = false;
        if (auto *shared = m_shared.load())
        {
            // Wake the runner so it notices the stop request immediately.
            shared->req_seq.fetch_add(1, std::memory_order_release);
            flash_ipc::futex_wake(&shared->req_seq);
        }
        if (m_runner_thread.joinable())
            m_runner_thread.join();
    }

    if (auto *shared = m_shared.load())
    {
        // Invalidate but keep it mapped: the game thread may still write the heartbeat while
        // the process shuts down; unmapping here could crash it. The kernel unmaps at exit.
        shared->installed.store(0);
        shared->magic.store(0);
    }

    if (m_shmid >= 0)
    {
        if (shmctl(m_shmid, IPC_RMID, nullptr) == -1)
            utils::log("[Ipc::Remove] shmctl failed: {}\n", strerror(errno));
        m_shmid = -1;
    }
}

void Ipc::runner()
{
    flash_ipc::Shared *const m_shared = this->m_shared.load(); // stays mapped until exit
    uint32_t last = m_shared->req_seq.load(std::memory_order_acquire);

    while (m_running)
    {
        uint32_t seq = m_shared->req_seq.load(std::memory_order_acquire);
        if (seq == last)
        {
            flash_ipc::futex_wait(&m_shared->req_seq, last, 250);
            continue;
        }

        if (!m_running)
            break;

        // Work on a private copy; the slot is only written back once we're done.
        Message msg;
        std::memcpy(static_cast<void *>(&msg), static_cast<const void *>(&m_shared->msg), sizeof(Message));

        // protocol-level health check: never queue work for a game thread that isn't ticking
        ResultCode status = flash_ipc::game_thread_alive(m_shared) ? m_handler(msg) : ResultCode::NOT_READY;

        std::memcpy(static_cast<void *>(&m_shared->msg), static_cast<const void *>(&msg), sizeof(Message));
        m_shared->status.store(static_cast<uint32_t>(status), std::memory_order_relaxed);
        m_shared->resp_seq.store(seq, std::memory_order_release);
        flash_ipc::futex_wake(&m_shared->resp_seq);

        last = seq;
    }
    utils::log("[Ipc::runner] Stopped\n");
}

Ipc::~Ipc()
{
    Remove();
}
