#ifndef DARKORBIT_H
#define DARKORBIT_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include <future>
#include <mutex>
#include <functional>
#include <unordered_map>
#include <utility>

#include "flash_stuff.h"
#include "utils.h"
#include "singleton.h"
#include "ipc.h"
#include "avm.h"


namespace game
{

class InfoHolder : public avm::ScriptObject
{
public:
    uint8_t pad0[0x28 - sizeof(avm::ScriptObject)];
    uintptr_t name;
};

class Ship : public avm::ScriptObject
{
public:
    struct LocationInfo
    {
        uint8_t pad0[0x20];
        double x;
        double y;
    };

    struct InfoHolder
    {
        struct Info
        {
            uint8_t pad0[0x28];
            avm::String *name;
        };
        uint8_t pad0[0x40];
        Info *info;

    };

    uint8_t pad0[0x38 - sizeof(avm::ScriptObject)];
    uint32_t id;
    uint32_t pad1;
    LocationInfo *location_info;

    uint8_t pad2[0x70 - 0x48];
    uint32_t check;     // 0x70
    uint32_t visible;   // 0x74
    uint32_t c;         // 0x78
    uint32_t d;         // 0x7c
    uint8_t pad3[0xf8 - 0x80];
    InfoHolder *info_holder;

    std::string name()
    {
        return (info_holder && info_holder->info) ? info_holder->info->name->read() : "INVALID!";
    }

    utils::vec2 position()
    {
        return (location_info) ? utils::vec2(location_info->x, location_info->y) : utils::vec2(-1, -1);
    }
};
};

class Darkorbit : public Singleton<Darkorbit>
{
public:

    typedef std::function<void(avm::MethodEnv *, uint32_t , uintptr_t *)> MyInvoke_t;

    struct FlashHook
    {
        avm::MethodInvoke_t envproc;
        avm::MethodInvoke_t infoproc;
        avm::MethodInvoke_t invoker;

        avm::MethodEnv *method = nullptr;
        avm::MethodInfo *method_info = nullptr;

        MyInvoke_t handler;

        // set by uninstall(); hook_proxy must not reinstall a removed hook
        bool removed = false;

        void restore()
        {
            if (method)
            {
                method->method_proc = envproc;
                method->method_info->method_proc = infoproc;
                method->method_info->invoker = invoker;
            }
            else
            {
                method_info->method_proc = infoproc;
                method_info->invoker = invoker;
            }

        }

        ~FlashHook() 
        {
        }
    };

    struct LateHook
    {
        avm::MethodInfo *method;
        MyInvoke_t handler;
    };


    bool install(uintptr_t main_address);
    bool uninstall();


    avm::String *create_string(const std::string &s)
    {
        auto *r = flash_stuff::newstring(m_main->core(), s);
        if (!r)
            return nullptr;

        /*
        r->composite |= 0x20000000; // Stack pin
        r->composite += 1; // Refcount
        */

        // Remove from zct
        r->composite &= ~0x80000000; // in zct

        auto *gc = avm::get_block_header(r)->gc;

        gc->zct_bottom[r->zct_index()] = 0;

        return r;
    }

    bool key_click(uint32_t key);

    bool mouse_click(int x, int y, int button);

    bool lock_entity(uint32_t id);

    bool refine_ore(uint32_t ore, uint32_t amount);

    bool use_item(const std::string &name, uint8_t type, uint8_t bar);

    bool send_notification(const std::string &name, const std::vector<Atom> &args);

    void hook_flash_function(avm::MethodEnv *method, MyInvoke_t handler);

    void hook_flash_function(avm::MethodInfo *method_info, MyInvoke_t handler);

    std::unordered_map<uint32_t, game::Ship *> get_ships();

    void notify_jit(avm::MethodInfo *method);

    void notify_freechunk(uintptr_t chunk);

    auto &get_hooks() { return m_hooks; }

    // Queues f for the game thread and returns its future (invalid if it can't be queued).
    // If *cancelled becomes true before the task runs, it's skipped.
    std::future<uintptr_t> call_sync(std::function<uintptr_t()> f, std::shared_ptr<std::atomic<bool>> cancelled = nullptr);

    // Fire-and-forget variant, returns false if the task couldn't be queued.
    bool post_async(std::function<uintptr_t()> f);

    // Hook proxy reentrancy tracking, see hook_proxy()
    void enter_proxy() { m_proxy_depth++; }
    void leave_proxy();

    avm::BuiltinType inline get_builtin_type(avm::Traits *traits)
    {
        return traits ? avm::BuiltinType(traits->builtinType) : avm::BUILTIN_any;
    }

    int check_method_signature(avm::ScriptObject *obj, int methodIdx, bool methodName, const std::string &signature);

    std::string get_method_signature(avm::MethodInfo *mi, bool method_name);

    // IPC request handler, runs on the ipc thread (see ipc_handler.cpp)
    flash_ipc::ResultCode handle_ipc_message(flash_ipc::Message &msg);



friend class Singleton;

private:

    Darkorbit() = default;
    Darkorbit &operator=(const Darkorbit) = delete;

    void handle_async_calls(avm::MethodEnv *env, uint32_t argc, uintptr_t *argv) ;

    // Max tasks waiting for the game thread; protects against unbounded growth when it stalls.
    static constexpr size_t MAX_PENDING_CALLS = 1024;

    std::unordered_map<uint32_t, FlashHook> m_hooks;
    int m_proxy_depth = 0;
    bool m_clear_hooks_pending = false;

    std::mutex m_call_mut;
    std::vector<std::packaged_task<uintptr_t()>> m_async_calls;

    Ipc m_ipc;
    std::atomic<bool> m_installed { false };

    uint32_t m_refine_multiname = 0;
    uint32_t m_item_prop_mn = 0;

    uintptr_t m_input_param = 0;

    avm::PoolObject     *m_const_pool = nullptr;

    avm::ScriptObject   *m_main =  nullptr,
                        *m_screen_manager = nullptr,
                        *m_event_manager = nullptr,
                        *m_gui_manager = nullptr;

    avm::ClassClosure   *m_menu_proxy = nullptr,
                        *m_action_closure = nullptr;

    avm::AbcEnv         *abc_env = nullptr;

};

#endif // DARKORBIT_H
