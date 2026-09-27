#ifndef FLASH_IPC_H
#define FLASH_IPC_H

// Shared-memory protocol between the bot client (DarkTanos.so, JVM side) and
// do_lib (preloaded into the flash process). Both sides include this header so
// the layout can never diverge.
//
// Protocol (single request slot, sequence numbered):
//   client: waits until resp_seq == req_seq (server idle), writes msg,
//           publishes req_seq + 1 and wakes the server futex.
//   server: waits for req_seq to change, copies msg out, processes it, writes
//           the response into msg, sets status and publishes resp_seq = req_seq.
//   client: waits (with timeout) for resp_seq to reach its sequence number.
// A client that times out never overwrites msg while the server is still busy
// with the previous request (req_seq != resp_seq), so late responses can't
// corrupt newer requests.

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <ctime>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace flash_ipc
{
    constexpr uint32_t MAGIC = 0x54414e32; // "TAN2"
    constexpr size_t SHM_SIZE = 4096;

    // Game thread is considered stalled if the timer hook didn't run for this long.
    constexpr uint64_t STALL_MS = 3000;

    // Server-side wait for the game thread to execute a synchronous call.
    constexpr int SERVER_CALL_TIMEOUT_MS = 2000;

    // Client-side wait for a response; must be larger than the server timeout so the
    // server normally answers (possibly with TIMEOUT) before the client gives up.
    constexpr int CLIENT_SYNC_TIMEOUT_MS = 2500;
    constexpr int CLIENT_ASYNC_TIMEOUT_MS = 250;

    // Async commands still waiting for the game thread after this long are dropped instead
    // of being executed in a late burst (e.g. after a hiccup of the game thread).
    constexpr uint64_t ASYNC_MAX_DELAY_MS = 1500;

    enum class MessageType : uint32_t
    {
        CALL,
        RESULT,
        SEND_NOTIFICATION,
        REFINE,
        UPGRADE,
        USE_ITEM,
        KEY_CLICK,
        MOUSE_CLICK,
        CHECK_SIGNATURE,
        NONE
    };

    enum class ResultCode : uint32_t
    {
        OK,
        TIMEOUT,     // game thread didn't execute the call in time
        NOT_READY,   // hooks not installed or game thread stalled
        BAD_REQUEST,
        QUEUE_FULL,
    };

    // Commands whose result the Java side ignores; the server acknowledges them as
    // soon as they're queued for the game thread instead of waiting for execution.
    inline bool is_async(MessageType type)
    {
        switch (type)
        {
            case MessageType::SEND_NOTIFICATION:
            case MessageType::REFINE:
            case MessageType::USE_ITEM:
            case MessageType::KEY_CLICK:
            case MessageType::MOUSE_CLICK:
                return true;
            default:
                return false;
        }
    }

    struct RefineMessage
    {
        MessageType type = MessageType::REFINE;
        uintptr_t refine_util;
        int ore, amount;
    };

    struct SendNotificationMessage
    {
        MessageType type = MessageType::SEND_NOTIFICATION;
        char name[64];
        uint32_t argc;
        uintptr_t argv[64];
    };

    struct FunctionResultMessage
    {
        MessageType type = MessageType::RESULT;
        bool error = false;
        uintptr_t value;
    };

    struct CallFunctionMessage
    {
        MessageType type = MessageType::CALL;
        uintptr_t object;
        uint32_t index;
        int argc;
        uintptr_t argv[64];
    };

    struct UseItemMessage
    {
        MessageType type = MessageType::USE_ITEM;
        char name[64];
        uint8_t action_type;
        bool action_bar;

        // ItemsControlMenuConstants.ACTION_SELECTION == 1
        // ItemsControlMenuConstants.ACTION_TOOGLE == 0
        // ItemsControlMenuConstants.ACTION_ONE_SHOT == 1
        // barId = _loc2_.barId == CATEGORY_BAR ? 0 : 1;
    };

    struct KeyClickMessage
    {
        MessageType type = MessageType::KEY_CLICK;
        uint32_t key;
    };

    struct MouseClickMessage
    {
        MessageType type = MessageType::MOUSE_CLICK;
        uint32_t button;
        int32_t x;
        int32_t y;
    };

    struct CheckSignatureMessage
    {
        MessageType type = MessageType::CHECK_SIGNATURE;
        uintptr_t object;
        uint32_t index;
        bool method_name;
        char signature[0x100];

        int32_t result;
    };

    union Message
    {
        Message() { }
        MessageType type = MessageType::NONE;
        CallFunctionMessage call;
        FunctionResultMessage result;
        SendNotificationMessage notify;
        RefineMessage refine;
        UseItemMessage item;
        KeyClickMessage key;
        MouseClickMessage click;
        CheckSignatureMessage sig;
    };

    struct Shared
    {
        std::atomic<uint32_t> magic;
        std::atomic<uint32_t> req_seq;      // futex word, client -> server
        std::atomic<uint32_t> resp_seq;     // futex word, server -> client
        std::atomic<uint32_t> status;       // ResultCode of the request identified by resp_seq
        std::atomic<uint64_t> heartbeat_ms; // CLOCK_MONOTONIC ms of the last game-thread timer tick
        std::atomic<uint32_t> installed;    // do_lib hooks are installed
        uint32_t reserved;
        Message msg;
    };

    static_assert(sizeof(Shared) <= SHM_SIZE, "Shared block is larger than the shared memory segment");
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "futex words must be lock free");
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "heartbeat must be lock free");

    inline uint64_t now_ms()
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000ull + static_cast<uint64_t>(ts.tv_nsec) / 1'000'000ull;
    }

    // Shared (non-private) futex ops: the words live in SysV shared memory mapped by two processes.
    inline void futex_wait(std::atomic<uint32_t> *addr, uint32_t expected, int timeout_ms)
    {
        timespec ts { timeout_ms / 1000, (timeout_ms % 1000) * 1'000'000L };
        syscall(SYS_futex, reinterpret_cast<uint32_t *>(addr), FUTEX_WAIT, expected, &ts, nullptr, 0);
    }

    inline void futex_wake(std::atomic<uint32_t> *addr)
    {
        syscall(SYS_futex, reinterpret_cast<uint32_t *>(addr), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
    }

    inline bool game_thread_alive(const Shared *shared, uint64_t max_age_ms = STALL_MS)
    {
        uint64_t hb = shared->heartbeat_ms.load(std::memory_order_acquire);
        return shared->installed.load(std::memory_order_acquire) && hb && now_ms() - hb < max_age_ms;
    }
}

#endif // FLASH_IPC_H
