// Tests the flash IPC with the production server (do_lib/ipc.cpp) and client
// (client/flash_ipc_client.cpp). A forked child plays the flash process: it runs the
// Ipc server plus a simulated game thread executing queued calls on every tick, like
// Darkorbit::handle_async_calls does inside the real game.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

#include "flash_ipc.h"
#include "flash_ipc_client.h"
#include "ipc.h"
#include "test_util.h"

using namespace std::chrono;
using flash_ipc::Message;
using flash_ipc::MessageType;
using flash_ipc::ResultCode;

namespace
{
    // control commands parent -> child over a pipe
    enum Control : char { STALL = 's', RESUME = 'r', UNINSTALL = 'u', INSTALL = 'i', QUIT = 'q' };

    // CALL requests: value = argv[0] * 2 + index, after sleeping argv[1] ms on the game thread.
    // index 999 returns the number of executed async commands.
    constexpr uint32_t INDEX_ASYNC_COUNT = 999;

    struct Game
    {
        std::mutex mutex;
        std::vector<std::packaged_task<uintptr_t()>> tasks;
        std::atomic<bool> stalled { false };
        std::atomic<bool> installed { true };
        std::atomic<uint64_t> async_executed { 0 };
    };

    ResultCode run_on_game(Game &game, std::function<uintptr_t()> f, bool wait, uintptr_t *out)
    {
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        std::packaged_task<uintptr_t()> task([f = std::move(f), cancelled] { return cancelled->load() ? 0 : f(); });
        auto fut = task.get_future();
        {
            std::lock_guard<std::mutex> lock(game.mutex);
            if (!game.installed || game.tasks.size() >= 1024) // mirrors Darkorbit::MAX_PENDING_CALLS
                return ResultCode::QUEUE_FULL;
            game.tasks.push_back(std::move(task));
        }
        if (!wait)
            return ResultCode::OK;

        if (fut.wait_for(milliseconds(flash_ipc::SERVER_CALL_TIMEOUT_MS)) != std::future_status::ready)
        {
            cancelled->store(true);
            return ResultCode::TIMEOUT;
        }
        try { *out = fut.get(); } catch (...) { return ResultCode::NOT_READY; }
        return ResultCode::OK;
    }

    [[noreturn]] void flash_process(int control_fd)
    {
        Game game;
        Ipc ipc;
        if (!ipc.Init())
            _exit(10);

        ipc.SetInstalled(true);
        ipc.Heartbeat();
        ipc.Run([&game](Message &msg) -> ResultCode
        {
            switch (msg.type)
            {
                case MessageType::CALL:
                {
                    auto call = msg.call;
                    uintptr_t value = 0;
                    ResultCode rc = run_on_game(game, [call, &game]() -> uintptr_t
                    {
                        if (call.index == INDEX_ASYNC_COUNT)
                            return game.async_executed.load();
                        if (call.argc > 1 && call.argv[1])
                            std::this_thread::sleep_for(milliseconds(call.argv[1]));
                        return call.argv[0] * 2 + call.index;
                    }, true, &value);
                    msg.result = {};
                    msg.result.error = rc != ResultCode::OK;
                    msg.result.value = value;
                    return rc;
                }
                case MessageType::CHECK_SIGNATURE:
                {
                    std::string sig(msg.sig.signature, strnlen(msg.sig.signature, sizeof(msg.sig.signature)));
                    uintptr_t value = 0;
                    ResultCode rc = run_on_game(game, [sig] { return static_cast<uintptr_t>(sig == "good"); }, true, &value);
                    msg.sig.result = rc == ResultCode::OK ? static_cast<int32_t>(value) : -1;
                    return rc;
                }
                case MessageType::KEY_CLICK:
                case MessageType::SEND_NOTIFICATION:
                    return run_on_game(game, [&game] { game.async_executed++; return 1; }, false, nullptr);
                default:
                    return ResultCode::BAD_REQUEST;
            }
        });

        // simulated game thread (~60 fps timer)
        std::thread game_thread([&]
        {
            while (true)
            {
                std::this_thread::sleep_for(milliseconds(16));
                if (game.stalled)
                    continue;
                ipc.Heartbeat();

                std::vector<std::packaged_task<uintptr_t()>> tasks;
                {
                    std::lock_guard<std::mutex> lock(game.mutex);
                    tasks.swap(game.tasks);
                }
                for (auto &t : tasks)
                    t();
            }
        });
        game_thread.detach();

        char c;
        while (read(control_fd, &c, 1) == 1)
        {
            switch (c)
            {
                case STALL: game.stalled = true; break;
                case RESUME: game.stalled = false; break;
                case UNINSTALL:
                {
                    game.installed = false;
                    ipc.SetInstalled(false);
                    std::lock_guard<std::mutex> lock(game.mutex);
                    game.tasks.clear();
                    break;
                }
                case INSTALL: game.installed = true; ipc.SetInstalled(true); break;
                case QUIT: ipc.Remove(); _exit(0);
            }
        }
        _exit(0);
    }

    struct FlashProcess
    {
        pid_t pid = -1;
        int control = -1;

        void start()
        {
            int fds[2];
            if (pipe(fds) != 0) abort();
            pid = fork();
            if (pid == 0)
            {
                close(fds[1]);
                flash_process(fds[0]);
            }
            close(fds[0]);
            control = fds[1];

            // wait until the segment exists
            for (int i = 0; i < 200 && shmget(pid, 0, 0) < 0; i++)
                std::this_thread::sleep_for(milliseconds(5));
        }

        void send(Control c) { if (write(control, &c, 1) != 1) abort(); }

        void quit()
        {
            send(QUIT);
            waitpid(pid, nullptr, 0);
            close(control);
        }
    };

    Message call_message(uintptr_t value, uint32_t index, uintptr_t sleep_ms = 0)
    {
        Message m;
        m.call = {};
        m.call.object = 1;
        m.call.index = index;
        m.call.argc = 2;
        m.call.argv[0] = value;
        m.call.argv[1] = sleep_ms;
        return m;
    }

    Message key_message(uint32_t key)
    {
        Message m;
        m.key = {};
        m.key.key = key;
        return m;
    }

    bool call(FlashIpcClient &client, pid_t pid, uintptr_t value, uint32_t index, uintptr_t *out, uintptr_t sleep_ms = 0)
    {
        Message resp;
        if (!client.Send(pid, call_message(value, index, sleep_ms), &resp) || resp.result.error)
            return false;
        *out = resp.result.value;
        return true;
    }

    uint64_t async_count(FlashIpcClient &client, pid_t pid)
    {
        uintptr_t v = 0;
        call(client, pid, 0, INDEX_ASYNC_COUNT, &v);
        return v;
    }
}

int main()
{
    test::Suite suite("flash ipc");

    FlashProcess flash;
    flash.start();
    FlashIpcClient client;

    suite.run("sync call returns the game thread result", [&]
    {
        uintptr_t v = 0;
        CHECK(call(client, flash.pid, 21, 3, &v));
        CHECK_EQ(v, 45u);
    });

    suite.run("check signature: valid, invalid, and never garbage", [&]
    {
        Message m;
        m.sig = {};
        m.sig.object = 1;
        m.sig.result = 12345;
        strcpy(m.sig.signature, "good");
        Message resp;
        CHECK(client.Send(flash.pid, m, &resp));
        CHECK_EQ(resp.sig.result, 1);
        strcpy(m.sig.signature, "bad");
        CHECK(client.Send(flash.pid, m, &resp));
        CHECK_EQ(resp.sig.result, 0);
    });

    suite.run("sync call latency (game thread at 60 fps)", [&]
    {
        std::vector<double> ms;
        for (int i = 0; i < 60; i++)
        {
            auto t0 = steady_clock::now();
            uintptr_t v = 0;
            CHECK(call(client, flash.pid, i, 1, &v));
            CHECK_EQ(v, static_cast<uintptr_t>(i * 2 + 1));
            ms.push_back(duration<double, std::milli>(steady_clock::now() - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        test::info("sync call: p50 %.2f ms, p95 %.2f ms, max %.2f ms (bounded by the 16 ms game tick)",
                   ms[ms.size() / 2], ms[ms.size() * 95 / 100], ms.back());
        CHECK(ms.back() < 100);
    });

    suite.run("async commands don't wait for the game thread", [&]
    {
        uint64_t before = async_count(client, flash.pid);
        std::vector<double> us;
        for (int i = 0; i < 200; i++)
        {
            auto t0 = steady_clock::now();
            CHECK(client.Send(flash.pid, key_message(65)));
            us.push_back(duration<double, std::micro>(steady_clock::now() - t0).count());
        }
        std::sort(us.begin(), us.end());
        test::info("async command: p50 %.1f us, p95 %.1f us, max %.1f us", us[us.size() / 2], us[us.size() * 95 / 100], us.back());
        CHECK(us[us.size() / 2] < 1000);

        // all of them execute, in order, on the next ticks
        std::this_thread::sleep_for(milliseconds(100));
        CHECK_EQ(async_count(client, flash.pid) - before, 200u);
    });

    suite.run("queue overflow (stalled tick) is rejected quickly, never blocks", [&]
    {
        flash.send(STALL);
        std::this_thread::sleep_for(milliseconds(20));
        int accepted = 0;
        auto t0 = steady_clock::now();
        for (int i = 0; i < 1500; i++)
            accepted += client.Send(flash.pid, key_message(66));
        double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
        test::info("1500 commands while the tick is paused: %d accepted, %.1f ms total", accepted, ms);
        CHECK(accepted >= 1000 && accepted <= 1024);
        CHECK(ms < 1000);
        flash.send(RESUME);
        std::this_thread::sleep_for(milliseconds(100));
    });

    suite.run("concurrent callers always get their own response", [&]
    {
        std::atomic<int> mismatches { 0 }, failures { 0 };
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; t++)
        {
            threads.emplace_back([&, t]
            {
                for (int i = 0; i < 40; i++)
                {
                    uintptr_t input = t * 100000 + i, v = 0;
                    if (!call(client, flash.pid, input, 7, &v)) { failures++; continue; }
                    if (v != input * 2 + 7) mismatches++;
                }
            });
        }
        for (auto &th : threads) th.join();
        CHECK_EQ(mismatches.load(), 0);
        CHECK_EQ(failures.load(), 0);
    });

    suite.run("stalled game thread fails fast instead of blocking the bot", [&]
    {
        flash.send(STALL);
        std::this_thread::sleep_for(milliseconds(flash_ipc::STALL_MS + 200));
        auto t0 = steady_clock::now();
        uintptr_t v = 0;
        CHECK(!call(client, flash.pid, 1, 1, &v));
        double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
        test::info("call with stalled game thread returned in %.3f ms", ms);
        CHECK(ms < 5);
        flash.send(RESUME);
        std::this_thread::sleep_for(milliseconds(50));
        CHECK(call(client, flash.pid, 1, 1, &v));
    });

    suite.run("game thread slower than the timeout: late result never leaks into the next call", [&]
    {
        uintptr_t v = 0;
        // takes longer than the server (2 s) and client (2.5 s) timeouts
        auto t0 = steady_clock::now();
        CHECK(!call(client, flash.pid, 5, 1, &v, 3000));
        double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
        test::info("timed out after %.0f ms", ms);
        CHECK(ms < flash_ipc::CLIENT_SYNC_TIMEOUT_MS + 200);

        // game thread is still busy for ~1s; calls fail but must never return wrong values
        int ok = 0;
        auto until = steady_clock::now() + milliseconds(2500);
        while (steady_clock::now() < until)
        {
            uintptr_t input = 1000 + ok;
            if (call(client, flash.pid, input, 2, &v))
            {
                CHECK_EQ(v, input * 2 + 2);
                ok++;
            }
        }
        CHECK(ok > 0);
    });

    suite.run("hooks uninstalled: requests rejected quickly, recovered after reinstall", [&]
    {
        flash.send(UNINSTALL);
        std::this_thread::sleep_for(milliseconds(50));
        auto t0 = steady_clock::now();
        uintptr_t v = 0;
        CHECK(!call(client, flash.pid, 1, 1, &v));
        CHECK(duration<double, std::milli>(steady_clock::now() - t0).count() < 5);
        flash.send(INSTALL);
        std::this_thread::sleep_for(milliseconds(50));
        CHECK(call(client, flash.pid, 4, 1, &v));
        CHECK_EQ(v, 9u);
    });

    suite.run("frozen game thread is detected", [&]
    {
        CHECK(!client.IsFrozen(flash.pid, 300));
        flash.send(STALL);
        std::this_thread::sleep_for(milliseconds(100));
        CHECK(!client.IsFrozen(flash.pid, 300)); // records the last heartbeat
        std::this_thread::sleep_for(milliseconds(400));
        CHECK(client.IsFrozen(flash.pid, 300));
        flash.send(RESUME);
        std::this_thread::sleep_for(milliseconds(50));
        CHECK(!client.IsFrozen(flash.pid, 300));
    });

    suite.run("not attached to a process without do_lib", [&]
    {
        FlashIpcClient other;
        auto t0 = steady_clock::now();
        CHECK(!other.Send(getpid(), key_message(1)));
        CHECK(duration<double, std::milli>(steady_clock::now() - t0).count() < 5);
    });

    suite.run("segment of a killed flash process is removed (no shm leak)", [&]
    {
        FlashProcess victim;
        victim.start();
        FlashIpcClient c;
        CHECK(c.Send(victim.pid, key_message(1)));
        kill(victim.pid, SIGKILL);
        waitpid(victim.pid, nullptr, 0);
        CHECK(shmget(victim.pid, 0, 0) >= 0); // the leak the old code had
        c.Reset(victim.pid, true);
        CHECK(shmget(victim.pid, 0, 0) < 0);
        close(victim.control);
    });

    suite.run("segment is removed even if the client never attached", [&]
    {
        FlashProcess victim;
        victim.start();
        kill(victim.pid, SIGKILL);
        waitpid(victim.pid, nullptr, 0);
        FlashIpcClient c;
        c.Reset(victim.pid, true);
        CHECK(shmget(victim.pid, 0, 0) < 0);
        close(victim.control);
    });

    suite.run("server shutdown removes its segment", [&]
    {
        pid_t pid = flash.pid;
        flash.quit();
        CHECK(shmget(pid, 0, 0) < 0);
    });

    return suite.finish();
}
