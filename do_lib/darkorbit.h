#ifndef DARKORBIT_H
#define DARKORBIT_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include <future>
#include <mutex>
#include <functional>
#include <map>
#include <string>
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

    // The game never unregisters the tooltips of action bar elements it throws away, the
    // TooltipControl singleton keeps them (and their whole display tree) forever. Remove the
    // tooltips of objects that left the stage with the game's own removeToolTip (game thread).
    void prune_tooltips();
    bool resolve_tooltip_control();

    struct TooltipFix
    {
        bool disabled = false;
        avm::ScriptObject *control = nullptr;     // TooltipControl._instance (lives in a static)
        avm::Multiname *list_mn = nullptr;        // TooltipControl.toolTipsList
        uint32_t get_target_index = 0;            // vtable index of ToolTipHook.getTargetObject
        avm::Multiname *stage_mn = nullptr;       // public DisplayObject.stage
        uint32_t remove_index = 0;                // vtable index of TooltipControl.removeToolTip
        uint64_t last_run = 0;
        uint64_t removed_total = 0;
    } m_tooltips;

    // ShipUpgradeDisplay (upgrade icons in a ship's HUD) subscribes to the map's
    // lockManager.targetChanged and never unsubscribes: every ship that dies stays alive with its
    // HUD, traits and 3D model until the map is left. Remove the listeners of displays whose ship
    // left the map with the game's own Slot.remove() (game thread).
    void prune_ship_listeners();
    bool resolve_ship_fix();

    struct ShipFix
    {
        bool disabled = false;
        avm::ScriptObject *screen = nullptr;      // ScreenManager (m_screen_manager, checked)
        uint32_t handler_index = 0;               // vtable index of ShipUpgradeDisplay.handleLockChanged
        uint32_t remove_index = 0;                // vtable index of Slot.remove
        uint64_t last_run = 0;
        uint64_t removed_total = 0;
    } m_ships;

    // GreenSock 11 (bundled with the game) takes a finished tween out of its timeline's list but
    // leaves the tween's own nextNode/prevNode pointing at its old neighbours. Whatever keeps one
    // finished tween keeps those alive, they keep theirs, ... and each tween its onComplete
    // closure: one idle ship's MoveTrait tween holds a chain of dead ships until the map is left.
    // Right before TweenLite.collectRetiredTweens (after the frame's rendering, the only time
    // GreenSock walks old links) clear the links of the finished tweens in masterList and of the
    // finished tweens they lead to.
    void unlink_retired_tweens();
    bool resolve_tween_fix();

    struct TweenFix
    {
        bool disabled = false;
        avm::ScriptObject *tween_lite = nullptr;  // TweenLite$ (class object: the statics)
        uint64_t unlinked_total = 0;
        uint64_t unlinked_unlogged = 0;
        uint64_t last_log = 0;
    } m_tweens;

    // away3d (bundled with the game): PrefabView3D.playAnimation() listens for PLAYBACK_COMPLETE on
    // the node animator.play() returns. That node belongs to the model's animation set, which every
    // view of the model shares, and the listener only goes away when the animation completes: never
    // for the looping idle animation. Every ship view ever shown stays alive (meshes, materials)
    // while its model is loaded. Before PrefabView3D.dispose() remove the view's listener from the
    // nodes of its animators' animation sets.
    bool resolve_view_fix();
    void release_view_listeners(avm::ScriptObject *view);
    std::vector<avm::ScriptObject *> vector_objects(avm::ScriptObject *vec);

    struct ViewFix
    {
        bool disabled = false;
        bool resolved = false;
        avm::Multiname *handler_mn = nullptr;     // PrefabView3D.handleAnimPlaybackComplete (private)
        avm::ScriptObject *event_class = nullptr; // AnimationStateEvent$ (PLAYBACK_COMPLETE)
        uint64_t released_total = 0;
        uint64_t released_unlogged = 0;
        uint64_t last_log = 0;
    } m_views;

    // PrefabMaterialManager (one per 3D model in use) builds a prototype material only to ask its
    // texture types. The material subscribes to three static Settings3D signals, and the manager's
    // dispose() drops it without disposing it: every manager ever created leaves a material with
    // its shader passes behind. Dispose the prototype right before PrefabMaterialManager.dispose().
    bool resolve_material_fix();
    void dispose_material_prototype(avm::ScriptObject *manager);

    struct MaterialFix
    {
        bool disabled = false;
        bool resolved = false;
        uint64_t disposed_total = 0;
        uint64_t disposed_unlogged = 0;
        uint64_t last_log = 0;
    } m_materials;

    // OutOfRangeEffect3D (the markers shown when attacking out of range) makes up to three marker
    // meshes per event on its static shared plane and takes them off the screen after 2 s
    // (removeAndCount) without disposing them: each stays registered with the static geometry and
    // the shared material forever. Dispose the marker right before removeAndCount().
    bool resolve_marker_fix();
    void dispose_marker(avm::ScriptObject *mesh);

    struct MarkerFix
    {
        bool disabled = false;
        bool resolved = false;
        uint64_t disposed_total = 0;
        uint64_t disposed_unlogged = 0;
        uint64_t last_log = 0;
    } m_markers;

    // away3d's MultiPassMaterialBase (EntityXRayMaterial: ships drawn with the x-ray shader) listens
    // to the shared light picker for light changes and dispose() never stops it: every disposed
    // multi-pass material stays alive with its passes. Clear lightPicker (its setter removes the
    // listener) right before MaterialBase.dispose() of a multi-pass material.
    bool resolve_light_fix();
    void release_light_listener(avm::ScriptObject *material);

    struct LightFix
    {
        bool disabled = false;
        bool resolved = false;
        avm::Traits *multi_pass = nullptr;        // MultiPassMaterialBase (instance traits)
        avm::Multiname *setter_mn = nullptr;      // MultiPassMaterialBase.lightPicker (setter)
        uint64_t released_total = 0;
        uint64_t released_unlogged = 0;
        uint64_t last_log = 0;
    } m_lights;

    // Value of / write to a var or const obj's class (or a base class) declares, by name
    Atom read_slot(avm::ScriptObject *obj, const char *name);
    bool write_slot(avm::ScriptObject *obj, const char *name, Atom value);
    avm::Multiname *slot_name(avm::ScriptObject *obj, const char *name);
    // slot names resolved per class (declared by it or a base class); nullptr: none
    std::map<std::pair<avm::Traits *, std::string>, avm::Multiname *> m_slot_names;

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

    avm::GC             *m_gc = nullptr;

};

#endif // DARKORBIT_H
