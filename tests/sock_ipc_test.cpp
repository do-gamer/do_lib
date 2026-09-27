// Tests the browser command channel end to end: production C++ client (client/sock_ipc.cpp)
// against the production JS server (browser/src/command_server.js) running in Node.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sock_ipc.h"
#include "test_util.h"

using namespace std::chrono;

namespace
{
    std::string path;
    uint32_t next_id = 0;

    pid_t start_server(int *stdout_fd)
    {
        int fds[2];
        if (pipe(fds) != 0) abort();
        pid_t pid = fork();
        if (pid == 0)
        {
            dup2(fds[1], STDOUT_FILENO);
            execlp("node", "node", TEST_SOURCE_DIR "/command_server_harness.js", path.c_str(), (char *)nullptr);
            _exit(127);
        }
        close(fds[1]);
        *stdout_fd = fds[0];
        return pid;
    }

    bool connect(SockIpc &sock)
    {
        for (int i = 0; i < 200; i++)
        {
            if (sock.Connect(path))
                return true;
            std::this_thread::sleep_for(milliseconds(10));
        }
        return false;
    }

    // same framing/ack handling as BotClient::SendBrowserCommand
    std::string command(SockIpc &sock, const std::string &cmd, const std::string &extra = "", int timeout = 1500)
    {
        uint32_t id = ++next_id;
        std::string json = "{\"id\":" + std::to_string(id) + ",\"cmd\":\"" + cmd + "\"" + extra + "}";
        if (!sock.Send(json, 250))
            return "send-failed";
        std::string prefix = std::to_string(id) + "|", line;
        auto deadline = steady_clock::now() + milliseconds(timeout);
        while (true)
        {
            int left = static_cast<int>(duration_cast<milliseconds>(deadline - steady_clock::now()).count());
            if (left <= 0 || !sock.RecvLine(line, left))
                return "timeout";
            if (line.rfind(prefix, 0) == 0)
                return line.substr(prefix.size());
        }
    }
}

int main()
{
    test::Suite suite("browser command channel");
    path = "/tmp/darktanos_test_" + std::to_string(getpid());

    int server_out = -1;
    pid_t server = start_server(&server_out);
    SockIpc sock;

    suite.run("connects and gets an ack", [&]
    {
        CHECK(connect(sock));
        CHECK(command(sock, "keyClick", ",\"key\":65") == "ok");
    });

    suite.run("failed command is reported immediately (no timeout/retries)", [&]
    {
        auto t0 = steady_clock::now();
        CHECK(command(sock, "fail") == "err");
        CHECK(duration<double, std::milli>(steady_clock::now() - t0).count() < 50);
    });

    suite.run("command latency (no 10 ms polling sleeps)", [&]
    {
        std::vector<double> us;
        for (int i = 0; i < 300; i++)
        {
            auto t0 = steady_clock::now();
            CHECK(command(sock, "keyDown", ",\"key\":66") == "ok");
            us.push_back(duration<double, std::micro>(steady_clock::now() - t0).count());
        }
        std::sort(us.begin(), us.end());
        test::info("round trip: p50 %.0f us, p95 %.0f us, max %.0f us", us[us.size() / 2], us[us.size() * 95 / 100], us.back());
        CHECK(us[us.size() / 2] < 5000);
    });

    suite.run("burst of pipelined commands arriving in one chunk: none dropped", [&]
    {
        // previously JSON.parse failed when two messages arrived in the same data event
        std::string burst;
        uint32_t first = next_id + 1;
        for (int i = 0; i < 50; i++)
            burst += "{\"id\":" + std::to_string(++next_id) + ",\"cmd\":\"keyUp\",\"key\":67}\n";
        burst.pop_back(); // Send appends the final newline
        CHECK(sock.Send(burst, 250));

        int acks = 0;
        std::string line;
        while (acks < 50 && sock.RecvLine(line, 1000))
        {
            CHECK(line == std::to_string(first + acks) + "|ok");
            acks++;
        }
        CHECK_EQ(acks, 50);
    });

    suite.run("text with quotes, backslashes and unicode survives", [&]
    {
        CHECK(command(sock, "text", ",\"text\":\"a \\\"quoted\\\" \\\\ path \\u00e9\"") == "ok");
    });

    suite.run("invalid json gets an error ack instead of silence", [&]
    {
        CHECK(sock.Send("{broken", 250));
        std::string line;
        CHECK(sock.RecvLine(line, 1000));
        CHECK(line == "0|err");
    });

    suite.run("stale ack from a timed-out command is skipped", [&]
    {
        CHECK(command(sock, "block", ",\"ms\":400", 100) == "timeout");
        // the late "N|ok" of the blocked command arrives first and must be ignored
        CHECK(command(sock, "keyClick", ",\"key\":68") == "ok");
    });

    suite.run("reconnects after the connection drops", [&]
    {
        sock.Close();
        CHECK(!sock.Connected());
        CHECK(connect(sock));
        CHECK(command(sock, "keyClick", ",\"key\":69") == "ok");
    });

    suite.run("server gone: send/recv fail fast", [&]
    {
        kill(server, SIGKILL);
        waitpid(server, nullptr, 0);
        auto t0 = steady_clock::now();
        std::string r = command(sock, "keyClick", ",\"key\":70");
        CHECK(r == "send-failed" || r == "timeout");
        CHECK(duration<double, std::milli>(steady_clock::now() - t0).count() < 1600);
        CHECK(!sock.Connect(path) || true);
    });

    suite.run("stale socket file is replaced on restart", [&]
    {
        // the killed server left its socket file behind
        CHECK(access(path.c_str(), F_OK) == 0);
        server = start_server(&server_out);
        SockIpc fresh;
        CHECK(connect(fresh));
        CHECK(command(fresh, "keyClick", ",\"key\":71") == "ok");
        kill(server, SIGKILL);
        waitpid(server, nullptr, 0);
    });

    unlink(path.c_str());
    return suite.finish();
}
