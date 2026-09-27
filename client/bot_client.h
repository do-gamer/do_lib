#ifndef BOT_CLIENT_H
#define BOT_CLIENT_H
#include <memory>
#include <mutex>
#include <initializer_list>
#include <string>
#include <string_view>
#include <queue>
#include <tuple>
#include <atomic>
#include <cstdint>
#include <vector>
#include "proc_util.h"
#include "flash_ipc.h"
#include "flash_ipc_client.h"

class SockIpc;

struct JsonParam
{
    const char *key;
    std::string_view value;
};

class BotClient
{
public:
    BotClient();
    ~BotClient();

    void SetCredentials(const std::string &sid, const std::string &url)
    {
        std::lock_guard<std::recursive_mutex> lock(m_browser_mutex);
        m_sid = sid;
        m_url = url;
    }

    void Refresh();
    void LaunchBrowser();
    // stops the browser for good (JVM shutdown)
    void Shutdown();

    void SetPid(int pid) { m_browser_pid = pid; }
    void SetFlashPid(int pid) { m_flash_pid = pid; }
    inline int Pid() const { return m_browser_pid; }
    inline int FlashPid() const { return m_flash_pid; }

    bool IsValid();

    bool SendBrowserCommand(const std::string &cmd, std::initializer_list<JsonParam> params = {});
    void ToggleBrowserVisibility(bool visible);

    // returns true if the command was successfully processed by flash
    bool SendFlashCommand(const flash_ipc::Message &message, flash_ipc::Message *response = nullptr);

    bool RefineOre(uintptr_t refine_util, uint32_t ore, uint32_t amount);
    bool SendNotification(uintptr_t screen_manager, const std::string &name, const std::vector<uintptr_t> &args);
    bool UseItem(const std::string &name, uint8_t type, uint8_t bar);
    uintptr_t CallMethod(uintptr_t obj, uint32_t index, const std::vector<uintptr_t> &args);
    bool KeyClickLegacy(uint32_t key);
    void KeyClick(uint32_t key);
    void KeyDown(uint32_t key);
    void KeyUp(uint32_t key);
    void SendText(const std::string &text);
    bool MouseClickLegacy(int32_t x, int32_t y);
    void MouseClick(int32_t x, int32_t y);
    void MouseMove(int32_t x, int32_t y);
    void MouseDown(int32_t x, int32_t y);
    void MouseUp(int32_t x, int32_t y);
    void MouseScroll(int32_t x, int32_t y, int32_t delta);
    int CheckMethodSignature(uintptr_t object, uint32_t index, bool check_name, const std::string &sig);

    // batch processing of native actions coming from the Java layer
    void PostActions(const std::vector<uint64_t> &actions);

    // paste text with optional before/after actions; thread‑safe queuing
    void PasteText(const std::string &text, const std::vector<uint64_t> &actions);

    // testing helper - show a red dot at the virtual cursor position
    void EnableCursorMarker(bool enable);
    void UpdateCursorMarker(int32_t x, int32_t y);

    // utility templates that are used by JNI wrapper; keep public so the JNI code can call them
    template <typename T>
    T Read(uintptr_t address, int *result = nullptr)
    {
        T r;
        ssize_t n = ProcUtil::ReadMemoryBytes(m_flash_pid, address, &r, sizeof(T));
        if (result)
        {
            *result = static_cast<int>(n);
        }
        if (n != static_cast<ssize_t>(sizeof(T)))
        {
            return T{};
        }
        return r;
    }

    template <typename T>
    bool Write(uintptr_t address, T value, int *result = nullptr)
    {
        ssize_t n = ProcUtil::WriteMemoryBytes(m_flash_pid, address, &value, sizeof(T));
        if (result)
        {
            *result = static_cast<int>(n);
        }
        return n == static_cast<ssize_t>(sizeof(T));
    }

    std::vector<uintptr_t> QueryMemory(const uint8_t *query, size_t size, size_t amount)
    {
        if (m_flash_pid < 0 && !find_flash_process())
        {
            return { };
        }
        return ProcUtil::QueryMemory(m_flash_pid, query, size, amount);
    }

private:
    std::unique_ptr<SockIpc> m_browser_ipc;
    uint32_t m_browser_cmd_id = 0;

    std::string m_sid;
    std::string m_url;
    std::string m_browser_ipc_path;

    // flash ipc (shared memory created by do_lib inside the flash process)
    FlashIpcClient m_flash_ipc;

    std::atomic<int> m_browser_pid { -1 }, m_flash_pid { -1 };

    uint64_t m_last_launch_ms = 0;
    bool m_launched_with_fuse = false;
    bool m_want_browser = false; // createWindow was called: keep the browser running
    bool m_force_extract_and_run = false;
    uint64_t m_last_flash_scan_ms = 0;

    // browser lifecycle + browser socket (recursive: restart paths call each other)
    std::recursive_mutex m_browser_mutex;

    // protects PostActions from concurrent invocation
    std::mutex m_post_actions_mutex;

    // queue used by PasteText
    std::mutex m_paste_mutex;
    std::queue<std::tuple<std::vector<uint64_t>, std::string, std::vector<uint64_t>>> m_paste_queue;
    std::atomic<bool> m_paste_worker_running{false};

    std::atomic<bool> m_last_valid { false };
    bool check_valid();

    bool find_flash_process();
    void reset();

    bool browser_alive();
    void kill_browser(bool reap_async = true);
    void restart_browser(const char *reason);
    bool maybe_relaunch_browser();

    // helpers for browser IPC
    bool ensure_browser_ipc_connected();
};


#endif /* BOT_CLIENT_H */
