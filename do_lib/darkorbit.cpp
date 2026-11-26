#include "darkorbit.h"
#include <algorithm>
#include <string>
#include <thread>
#include <iostream>
#include <sstream>

#include "disassembler.h"
#include "memory.h"
#include "offsets.h"


#define TAG_NUMBER(val) (uintptr_t(val) << 3) | 6


// Proxy flash calls to our handlers
uintptr_t hook_proxy(avm::MethodEnv *env, uint32_t argc, uintptr_t *argv)
{
    auto &darkorbit = Darkorbit::get();
    auto &hooks = darkorbit.get_hooks();
    const uint32_t id = env->method_info->id;

    auto it = hooks.find(id);
    if (it == hooks.end())
    {
        // Not ours anymore (restored by uninstall), just forward to the original
        return (!(argv[0] & 7)) ? env->method_info->method_proc(env, argc, argv)
                                : env->method_info->invoker(env, argc, argv);
    }

    // unordered_map references stay valid across inserts; erasing is deferred while
    // any proxy is on the stack (see uninstall), so this reference can't dangle.
    Darkorbit::FlashHook &hook = it->second;
    darkorbit.enter_proxy();

    hook.method = env;

    // Restore invokers
    hook.restore();

    hook.handler(env, argc, argv);

    // Call original
    uintptr_t r = 0;
    Atom this_object = argv[0];
    if (!(this_object & 7))
    {
        r = env->method_info->method_proc(env, argc, argv);
    }
    else
    {
        r = env->method_info->invoker(env, argc, argv);
    }

    if (!hook.removed)
    {
        // Save potentially new invokers
        hook.envproc = env->method_proc;
        hook.infoproc = env->method_info->method_proc;
        hook.invoker = env->method_info->invoker;

        // Reinstall hook
        env->method_proc = hook_proxy;
        env->method_info->method_proc = hook_proxy;
        env->method_info->invoker = hook_proxy;
    }

    darkorbit.leave_proxy();
    return r;
}

void Darkorbit::leave_proxy()
{
    if (--m_proxy_depth == 0 && m_clear_hooks_pending)
    {
        m_clear_hooks_pending = false;
        for (auto it = m_hooks.begin(); it != m_hooks.end();)
            it = it->second.removed ? m_hooks.erase(it) : std::next(it);
    }
}

void Darkorbit::hook_flash_function(avm::MethodEnv *method, MyInvoke_t handler)
{
    FlashHook hook;

    auto mit = m_hooks.find(method->method_info->id);
    if (mit != m_hooks.end() && !mit->second.removed)
    {
        mit->second.restore();
    }

    hook.envproc = method->method_proc;
    hook.infoproc = method->method_info->method_proc;
    hook.invoker = method->method_info->invoker;
    hook.handler = std::move(handler);
    hook.method = method;

    m_hooks[method->method_info->id] = std::move(hook);

    method->method_proc = hook_proxy;
    method->method_info->method_proc = hook_proxy;
    method->method_info->invoker = hook_proxy;

}
void Darkorbit::hook_flash_function(avm::MethodInfo *method, MyInvoke_t handler)
{
    FlashHook hook;

    auto mit = m_hooks.find(method->id);
    if (mit != m_hooks.end() && !mit->second.removed)
    {
        mit->second.restore();
    }

    hook.envproc = method->method_proc;
    hook.infoproc = method->method_proc;
    hook.invoker = method->invoker;
    hook.handler = std::move(handler);
    hook.method_info = method;

    m_hooks[method->id] = std::move(hook);

    method->method_proc = hook_proxy;
    method->invoker = hook_proxy;

}

// Set TANOS_DEBUG=1 to log hook activity (diagnostics, off by default)
static const bool g_debug = getenv("TANOS_DEBUG") != nullptr;

// The main isolate's thread is the first to JIT anything. Flash Workers run their own VM (and
// GC) on other threads; their JIT/free events never concern our hooks and m_hooks isn't
// thread-safe, so only the game thread may touch it.
static bool is_game_thread()
{
    static const std::thread::id game_thread = std::this_thread::get_id();
    return std::this_thread::get_id() == game_thread;
}

// maybe use a global callback thingy to dispatch jit stuff
void Darkorbit::notify_jit(avm::MethodInfo *method)
{
    if (!is_game_thread())
        return;

    if (g_debug)
    {
        static std::atomic<uint32_t> count { 0 };
        uint32_t n = ++count;
        std::string name = method->name();
        if (n <= 20 || n % 1000 == 0 || name.find("autoStartEnabled") != std::string::npos)
            utils::log("[debug] jit #{}: {}\n", n, name);
    }

    if (!m_installed && method->name().find("autoStartEnabled") != std::string::npos)
    {
        hook_flash_function(method, [this] (avm::MethodEnv *env, uint32_t argc, uintptr_t *argv)
        {
            install(avm::remove_kind(argv[0]));
            return 0L;
        });
    }
}

void Darkorbit::notify_freechunk(uintptr_t chunk)
{
    if (g_debug)
    {
        static std::atomic<uint32_t> count { 0 };
        uint32_t n = ++count;
        if (n == 1 || n % 10000 == 0)
            utils::log("[debug] free_chunk #{}\n", n);
    }

    // Invalidate VM-pointer keyed caches (O(1), this runs inside the GC free path)
    avm::clear_traits_cache();

    if (!is_game_thread())
        return;

    for (auto &[id, hook] : m_hooks)
    {
        if (!hook.removed && hook.method && (reinterpret_cast<uintptr_t>(hook.method) & ~0xfff) == chunk)
        {
            if (m_installed)
            {
                // uninstall() modifies m_hooks, never keep iterating after it
                uninstall();
                return;
            }

            // Not installed (e.g. the autoStartEnabled hook of a failed install): restore it
            // while the memory is still valid and forget it, so nothing writes to it later.
            hook.restore();
            hook.removed = true;
            m_clear_hooks_pending = true;
        }
    }

    if (m_clear_hooks_pending && m_proxy_depth == 0)
    {
        m_clear_hooks_pending = false;
        for (auto it = m_hooks.begin(); it != m_hooks.end();)
            it = it->second.removed ? m_hooks.erase(it) : std::next(it);
    }
}

std::unordered_map<uint32_t, game::Ship *> Darkorbit::get_ships()
{
    std::unordered_map<uint32_t, game::Ship *> r;

    auto ships = m_screen_manager->get_at<uintptr_t>(0x100, 0x28);
    auto elements = memory::read<uintptr_t>(ships + 0x30);
    auto size = memory::read<uint32_t>(ships + 0x38);

    for (size_t i = 0; i < size; i++)
    {
        auto *ship = avm::remove_kind(memory::read<game::Ship *>(elements + 0x10 + i * 8));
        if (ship && flash_stuff::hasproperty(ship, "pet"))
        {
            r[ship->id] = ship;
        }
    }
    return r;
}

std::future<uintptr_t> Darkorbit::call_sync(std::function<uintptr_t()> f, std::shared_ptr<std::atomic<bool>> cancelled)
{
    std::packaged_task<uintptr_t()> task([f = std::move(f), cancelled = std::move(cancelled)] () -> uintptr_t
    {
        if (cancelled && cancelled->load())
            return 0;
        return f();
    });
    std::future<uintptr_t> fut = task.get_future();

    std::scoped_lock lk { m_call_mut };
    if (!m_installed || m_async_calls.size() >= MAX_PENDING_CALLS)
        return {};

    m_async_calls.emplace_back(std::move(task));
    return fut;
}

bool Darkorbit::post_async(std::function<uintptr_t()> f)
{
    const uint64_t queued = flash_ipc::now_ms();
    return call_sync([f = std::move(f), queued]() -> uintptr_t
    {
        if (flash_ipc::now_ms() - queued > flash_ipc::ASYNC_MAX_DELAY_MS)
            return 0; // stale input, dropping is safer than acting on an old game state
        return f();
    }).valid();
}

static inline bool is_object_atom(Atom a) { return (a & 7) == 1 && (a & ~Atom(7)) != 0; }
static constexpr Atom NULL_ATOM = 1;  // kObjectType without a pointer

static inline avm::ScriptObject *as_object(Atom a)
{
    return is_object_atom(a) ? reinterpret_cast<avm::ScriptObject *>(avm::remove_kind(a)) : nullptr;
}

// Multiname of a var or const declared by these traits or a base class' (first match from the
// most derived class), from the declaring class' own pool: exact, private namespaces included
static avm::Multiname *declared_slot(avm::Traits *traits, const std::string &name)
{
    for (int depth = 0; traits && depth < 32; traits = traits->base, depth++)
    {
        if (!traits->pool)
            continue;
        for (auto &trait : traits->parse_traits().traits)
        {
            if ((trait.kind == avm::TRAIT_Slot || trait.kind == avm::TRAIT_Const) && trait.name == name)
                return traits->pool->get_multiname(trait.name_index);
        }
    }
    return nullptr;
}

// vtable index of the method declared with exactly this name (0: none). Calling methods instead
// of reading properties: a wrong name can't make the VM throw through our frames.
static uint32_t method_index(avm::ScriptObject *obj, const std::string &name)
{
    auto methods = obj->vtable->get_methods();
    for (uint32_t i = 0; i < methods.size(); i++)
    {
        if (methods[i] && methods[i]->method_info && methods[i]->method_info->name() == name)
            return i;
    }
    return 0;
}

// QName (flag 0x02 only: no ATTR, RTNS, RTNAME, NSSET) in the public namespace of a package
static avm::Multiname *public_qname(avm::PoolObject *pool, const std::string &uri, const std::string &name)
{
    for (uintptr_t i = 0; i < pool->precomp_mn_size; i++)
    {
        avm::Multiname *mn = pool->get_multiname(i);
        if (mn && (mn->flags & 0x1f) == 0x02 && mn->ns && mn->ns->type() == avm::Namespace::NS_Public
            && mn->get_name() == name && mn->ns->get_uri() == uri)
            return mn;
    }
    return nullptr;
}

// Class object of a public game class by its name: the public QName with that name in a package
// (game classes all live in one). It must be in one package only: finddef/getproperty of a name
// that isn't a definition throw through our frames. nullptr with the reason in *why.
static avm::ScriptObject *find_class(avm::PoolObject *pool, avm::ScriptObject *main, const std::string &name,
                                     const char **why)
{
    auto fail = [why](const char *reason) -> avm::ScriptObject *
    {
        *why = reason;
        return nullptr;
    };
    avm::Multiname *class_mn = nullptr;
    for (uintptr_t i = 0; i < pool->precomp_mn_size; i++)
    {
        avm::Multiname *mn = pool->get_multiname(i);
        if (!mn || (mn->flags & 0x1f) != 0x02 || !mn->ns || mn->ns->type() != avm::Namespace::NS_Public
            || mn->get_name() != name || mn->ns->get_uri().empty())
            continue;
        if (class_mn && class_mn->ns->get_uri() != mn->ns->get_uri())
            return fail("class name in more than one package");
        class_mn = mn;
    }
    if (!class_mn)
        return fail("class not in the game's constant pool");
    avm::ScriptObject *script = flash_stuff::finddef(main->vtable->einit, class_mn);
    if (!script)
        return fail("class not defined");
    Atom class_atom = flash_stuff::get_property(script, class_mn);
    if (!is_object_atom(class_atom))
        return fail("class object not found");
    auto *cls = reinterpret_cast<avm::ScriptObject *>(avm::remove_kind(class_atom));
    if (cls->vtable->traits->name() != name + "$")   // class objects' traits: "Name$"
        return fail("unexpected class object");
    return cls;
}

bool Darkorbit::resolve_tooltip_control()
{
    auto &f = m_tooltips;
    auto disable = [&f](const char *why)
    {
        utils::log("[tooltip] cleaner disabled: {}\n", why);
        f.disabled = true;
        return false;
    };

    const char *why = nullptr;
    avm::ScriptObject *cls = find_class(m_const_pool, m_main, "TooltipControl", &why);
    if (!cls)
        return disable(why);

    // the game's own TooltipControl.getInstance() (static: the class object is 'this')
    uint32_t get_instance = method_index(cls, "getInstance");
    if (!get_instance)
        return disable("TooltipControl.getInstance not found");
    auto *control = avm::remove_kind(reinterpret_cast<avm::ScriptObject *>(cls->call(get_instance)));
    if (!control || control->vtable->traits->name() != "TooltipControl")
        return disable("unexpected TooltipControl instance");

    avm::Multiname *list_mn = declared_slot(control->vtable->traits, "toolTipsList");
    if (!list_mn)
        return disable("TooltipControl.toolTipsList not found");
    uint32_t remove_index = method_index(control, "removeToolTip");
    if (!remove_index)
        return disable("TooltipControl.removeToolTip not found");

    // DisplayObject.stage, read only on hook targets (InteractiveObjects, never anything else)
    avm::Multiname *stage_mn = public_qname(m_const_pool, "", "stage");
    if (!stage_mn)
        return disable("public 'stage' name not found");

    f.control = control;
    f.list_mn = list_mn;
    f.stage_mn = stage_mn;
    f.remove_index = remove_index;
    utils::log("[tooltip] cleaner ready (getInstance {}, removeToolTip {})\n", get_instance, remove_index);
    return true;
}

void Darkorbit::prune_tooltips()
{
    auto &f = m_tooltips;
    if (f.disabled || !m_main)
        return;

    const uint64_t now = flash_ipc::now_ms();
    if (now - f.last_run < 15000)
        return;
    f.last_run = now;

    if (!f.control && !resolve_tooltip_control())
        return;

    Atom list_atom = flash_stuff::get_property(f.control, f.list_mn);
    if (!is_object_atom(list_atom))
        return;
    auto *list = reinterpret_cast<avm::Array *>(avm::remove_kind(list_atom));
    if (list->vtable->traits->name() != "Array" || !list->data
        || avm::get_block_header(list->data)->gc != m_gc)  // the storage must be a block of our GC
    {
        utils::log("[tooltip] cleaner disabled: toolTipsList isn't an Array\n");
        f.disabled = true;
        return;
    }
    // dense storage: ListData { vtable, uint32 len, ..., Atom entries[] }
    const uint32_t dense_len = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(list->data) + 8);
    const uint32_t count = std::min(list->size, dense_len);

    // collect first: removeToolTip splices the list. The targets stay referenced by the list
    // until their own removal, so the GC can't free them meanwhile.
    std::vector<avm::ScriptObject *> stale;
    for (uint32_t i = 0; i < count && stale.size() < 150; i++)
    {
        Atom hook_atom = (*list)[i];
        if (!is_object_atom(hook_atom))
            continue;
        auto *hook = reinterpret_cast<avm::ScriptObject *>(avm::remove_kind(hook_atom));
        if (hook->vtable->traits->name() != "ToolTipHook")
            continue; // e.g. a subclass: leave it alone
        if (!f.get_target_index && !(f.get_target_index = method_index(hook, "getTargetObject")))
        {
            utils::log("[tooltip] cleaner disabled: ToolTipHook.getTargetObject not found\n");
            f.disabled = true;
            return;
        }
        auto *target = avm::remove_kind(reinterpret_cast<avm::ScriptObject *>(hook->call(f.get_target_index)));
        if (!target)
            continue;
        Atom stage = flash_stuff::get_property(target, f.stage_mn);
        if (stage == NULL_ATOM)
        {
            stale.push_back(target);
        }
        else if (!is_object_atom(stage) ||
                 reinterpret_cast<avm::ScriptObject *>(avm::remove_kind(stage))->vtable->traits->name() != "Stage")
        {
            utils::log("[tooltip] cleaner disabled: 'stage' didn't read a Stage\n");
            f.disabled = true;
            return;
        }
    }

    for (auto *target : stale)
        f.control->call(f.remove_index, target);   // TooltipControl.removeToolTip(target)

    if (!stale.empty())
    {
        f.removed_total += stale.size();
        utils::log("[tooltip] removed {} tooltips of objects off the stage ({} hooks before, {} total), GC heap {} MB\n",
                   stale.size(), count, f.removed_total, flash_stuff::gc_heap_used_mb());
    }
}

// Value of a slot obj's class (or a base class) declares, 0 if it declares none: getproperty of
// a name the object doesn't have would throw through our frames
Atom Darkorbit::read_slot(avm::ScriptObject *obj, const char *name)
{
    auto key = std::make_pair(obj->vtable->traits, std::string(name));
    auto it = m_ships.slots.find(key);
    if (it == m_ships.slots.end())
        it = m_ships.slots.emplace(key, declared_slot(key.first, key.second)).first;
    return it->second ? flash_stuff::get_property(obj, it->second) : 0;
}

bool Darkorbit::resolve_ship_fix()
{
    auto &f = m_ships;
    auto disable = [&f](const char *why)
    {
        utils::log("[ships] cleaner disabled: {}\n", why);
        f.disabled = true;
        return false;
    };

    if (!m_screen_manager || avm::get_block_header(m_screen_manager)->gc != m_gc
        || m_screen_manager->get_name() != "ScreenManager")
        return disable("unexpected ScreenManager");

    f.screen = m_screen_manager;
    utils::log("[ships] cleaner ready\n");
    return true;
}

void Darkorbit::prune_ship_listeners()
{
    auto &f = m_ships;
    if (f.disabled || !m_main)
        return;

    const uint64_t now = flash_ipc::now_ms();
    if (now - f.last_run < 15000)
        return;
    f.last_run = now;

    if (!f.screen && !resolve_ship_fix())
        return;

    // ScreenManager._map._lockManager.targetChanged.slots: the listener list (osflash SlotList)
    avm::ScriptObject *map = as_object(read_slot(f.screen, "_map"));
    if (!map || map->get_name() != "Map")
        return;
    avm::ScriptObject *lock = as_object(read_slot(map, "_lockManager"));
    if (!lock || lock->get_name() != "LockManager")
        return;
    avm::ScriptObject *signal = as_object(read_slot(lock, "targetChanged"));
    if (!signal || signal->get_name() != "Signal")
        return;

    // collect first, remove after: Slot.remove() replaces the list. Until its own removal each
    // collected Slot stays in the signal's list, so the GC can't free it meanwhile.
    std::vector<avm::ScriptObject *> stale;
    uint32_t count = 0;
    for (avm::ScriptObject *node = as_object(read_slot(signal, "slots")); node && count < 20000; count++)
    {
        if (node->get_name() != "SlotList" || read_slot(node, "nonEmpty") != avm::TRUE)
            break; // SlotList.NIL ends the list
        avm::ScriptObject *slot = as_object(read_slot(node, "head"));
        node = as_object(read_slot(node, "tail"));
        if (!slot || slot->get_name() != "Slot" || stale.size() >= 100)
            continue;
        avm::ScriptObject *closure = as_object(read_slot(slot, "_listener"));
        if (!closure || closure->get_name() != "MethodClosure")
            continue;
        // a bound method: its receiver, and the MethodEnv it calls
        const auto c = reinterpret_cast<uintptr_t>(closure);
        avm::ScriptObject *display = as_object(*reinterpret_cast<Atom *>(c + offsets::closure_saved_this));
        if (!display || display->get_name() != "ShipUpgradeDisplay")
            continue; // another listener (lock displays, HealthInfoDisplay, ...)
        if (!f.handler_index && !(f.handler_index = method_index(display, "handleLockChanged")))
        {
            utils::log("[ships] cleaner disabled: ShipUpgradeDisplay.handleLockChanged not found\n");
            f.disabled = true;
            return;
        }
        if (*reinterpret_cast<avm::MethodEnv **>(c + offsets::closure_call_env) != display->vtable->methods[f.handler_index])
            continue; // another method of the display
        avm::ScriptObject *trait = as_object(read_slot(display, "_trait"));
        avm::ScriptObject *owner = trait ? as_object(read_slot(trait, "_owner")) : nullptr;
        // MapEntity.deactivateEntity() (removed from the map) nulls _container
        if (owner && read_slot(owner, "_container") == NULL_ATOM)
            stale.push_back(slot);
    }
    if (stale.empty())
        return;

    if (!f.remove_index && !(f.remove_index = method_index(stale.front(), "remove")))
    {
        utils::log("[ships] cleaner disabled: Slot.remove not found\n");
        f.disabled = true;
        return;
    }
    for (avm::ScriptObject *slot : stale)
        slot->call(f.remove_index);   // Slot.remove(): targetChanged.remove(listener)

    f.removed_total += stale.size();
    utils::log("[ships] released {} dead ships (lock listeners: {} before, {} removed in total), GC heap {} MB\n",
               stale.size(), count, f.removed_total, flash_stuff::gc_heap_used_mb());
}

void Darkorbit::handle_async_calls(avm::MethodEnv *env, uint32_t argc, uintptr_t *argv)
{
    m_ipc.Heartbeat();

    // the game's own leaks, on the game thread. They call into the VM: a collection may free
    // hooked code meanwhile (free_chunk -> uninstall)
    prune_tooltips();
    if (!m_installed)
        return;
    prune_ship_listeners();
    if (!m_installed)
        return;

    // Run the tasks outside the lock: they call into the VM, which may re-enter us
    // (GC -> free_chunk -> uninstall) and the ipc thread must be able to queue meanwhile.
    std::vector<std::packaged_task<uintptr_t()>> tasks;
    {
        std::scoped_lock lk { m_call_mut };
        if (m_async_calls.empty())
            return;
        tasks.swap(m_async_calls);
    }

    for (auto &task : tasks)
    {
        if (!m_installed)
            break; // remaining tasks are dropped, their futures report broken_promise
        task();
    }
}

bool Darkorbit::mouse_click(int x, int y, int button)
{
    flash_stuff::mouse_press(x, y, button);
    flash_stuff::mouse_release(x, y, button);
    return true;
}

bool Darkorbit::key_click(uint32_t key)
{
    auto *kbmapper = m_event_manager ? m_event_manager->get_at<avm::ScriptObject *>(0x68) : nullptr;
    if (!kbmapper)
        return false;
    kbmapper->call(3, static_cast<Atom>(key));
    return true;
}

bool Darkorbit::lock_entity(uint32_t id)
{
    utils::log("[*] Trying to lock entity {}\n", id);
    auto facade = m_screen_manager ? m_screen_manager->get_at<avm::ScriptObject *>(0x100, 0x78, 0x28) : nullptr;
    if (!facade)
        return false;

    game::Ship *player = reinterpret_cast<game::Ship *>(avm::remove_kind(m_event_manager->call(7)));
    game::Ship *target_ship = get_ships()[id];

    if (target_ship && player)
    {
        std::array<uintptr_t, 8> array_args {
            TAG_NUMBER(target_ship->id),
            TAG_NUMBER(target_ship->position().x),
            TAG_NUMBER(target_ship->position().y),
            TAG_NUMBER(player->position().x),
            TAG_NUMBER(player->position().y),
            TAG_NUMBER(0),
            TAG_NUMBER(0),
            TAG_NUMBER(100 + rand() % 400), // radius
        };

        // Should call send_notification() but we leave it as is since it's not used anymore
        auto *arg_array = (avm::Array *)flash_stuff::newarray(facade->vtable->methods[0], array_args.size(), array_args.data());
        avm::String *notification = flash_stuff::newstring(facade->core(), "MapAssetNotificationTRY_TO_SELECT_MAPASSET");

        facade->call(8, notification, (uintptr_t)arg_array | 1);

        utils::log("[*] Locking {}", target_ship->name());
        return true;
    }
    return false;
}

bool Darkorbit::refine_ore(uint32_t ore, uint32_t amount)
{
    auto *refinement = m_gui_manager ? m_gui_manager->get_at<avm::ScriptObject *>(0x78) : nullptr;

    if (refinement)
    {
        if (!m_refine_multiname)
        {
            auto disass = Disassembler::Disassemble(refinement->vtable->methods[20]->method_info);
            for (uint32_t &xref : disass.GetXrefs())
            {
                auto *mn = m_const_pool->get_multiname(xref);
                if (mn && mn->ns->get_uri().find(".com.module") != std::string::npos)
                {
                    m_refine_multiname = xref;
                    utils::log("[+] Found multiname: {}::{} ffs\n", mn->ns->get_uri(), mn->get_name());
                    break;
                }
            }
        }

        if (m_refine_multiname)
        {
            auto *obj = flash_stuff::finddef(m_main->vtable->einit, m_const_pool->get_multiname(m_refine_multiname));
            auto *closure = obj ? obj->get_at<avm::ClassClosure *>(0x20) : nullptr;
            auto *instance = closure ? reinterpret_cast<avm::ScriptObject *>(closure->call(5)) : nullptr;
            auto *ore_info = instance ? instance->get_at<avm::ScriptObject *>(0x20) : nullptr;
            auto *ore_type = ore_info ? ore_info->get_at<avm::ScriptObject *>(0x20) : nullptr;
            auto *net = m_main->get_at<avm::ScriptObject *>(0x230);

            if (!ore_type || !net)
                return false;

            ore_info->write_at<double>(0x28, amount);
            ore_type->write_at<int>(0x20, ore);

            net->call(19, instance);
        }
    }

    return true;
}

bool Darkorbit::use_item(const std::string &name, uint8_t type, uint8_t bar)
{
    auto *net = m_main ? m_main->get_at<avm::ScriptObject *>(0x230) : nullptr;
    if (!m_action_closure || !net)
        return false;

    avm::String *name_str = create_string(name);
    avm::ScriptObject *instance = m_action_closure->construct();
    if (!name_str || !instance)
        return false;


    if (!m_item_prop_mn)
    {
        auto traits = instance->vtable->traits->parse_traits();
        for (const auto &slot : traits.get_slots())
        {
            avm::Multiname *type_mn = m_const_pool->get_multiname(slot.type_id);
            if (type_mn && type_mn->get_name() == "String")
            {
                m_item_prop_mn = slot.name_index;
                break;
            }
        }
    }

    if (!m_item_prop_mn)
    {
        instance->set_at(name_str, 0x28);
    }
    else
    {
        auto *mn = m_const_pool->get_multiname(m_item_prop_mn);
        // Use setproperty to make sure gc knows we're moving this struct here
        flash_stuff::setproperty(instance, mn, reinterpret_cast<Atom>(name_str) | 2);
    }
    instance->set_at(type, 0x20);
    instance->set_at(bar, 0x24);

    net->call(19, instance);

    return false;
}

bool Darkorbit::send_notification(const std::string &name, const std::vector<Atom> &args)
{
    auto facade = m_screen_manager ? m_screen_manager->get_at<avm::ScriptObject *>(0x100, 0x78, 0x28) : nullptr;
    if (!facade)
        return false;

    // no need to cache these, ref count is not increased
    auto *arg_array = reinterpret_cast<avm::Array *>(
        flash_stuff::newarray(facade->vtable->methods[0], static_cast<uint32_t>(args.size()), const_cast<Atom *>(args.data())));
    avm::String *notification = create_string("MapAssetNotificationTRY_TO_SELECT_MAPASSET");
    if (!arg_array || !notification)
        return false;

    facade->call(8, notification, (uintptr_t)arg_array | 1);

    return true;
}

int Darkorbit::check_method_signature(avm::ScriptObject *obj, int methodIdx, bool methodName, const std::string &signature)
{
    if (obj) {
        avm::MethodEnv *method = obj->vtable->methods[methodIdx];
        if (method && method->method_info) {
            std::string flashSignature = get_method_signature(method->method_info, methodName);

            utils::log("Signature: {} == {}\n", signature, flashSignature);
            return !flashSignature.empty() && flashSignature == signature;
        }
    }

    return -1;
}

std::string Darkorbit::get_method_signature(avm::MethodInfo *mi, bool method_name)
{
    std::stringstream ss;

    avm::MethodSignature *ms = flash_stuff::get_method_signature(mi);
    if (ms) {
        ss << get_builtin_type(ms->_returnTraits);

        if (method_name) {
            std::string mn = mi->name();
            if (mn.empty()) return "";

            ss << "(";
            auto index = mn.find('/');
            if (index != std::string::npos) {
                ss << mn.substr(index + 1);
            } else ss << mn;
            ss << ")";
        }

        ss << "(";
        for (int i = 0; i <= ms->param_count; i++) {
            auto bt = get_builtin_type(ms->paramTraits(i));

            ss << bt;
            if (i > ms->param_count - ms->optional_count) {
                ss << "?";
            }
        }

        ss << ")" << ms->param_count << ms->optional_count << ms->rest_offset << ms->max_stack
        << ms->local_count << ms->max_scope << ms->frame_size << ms->isNative << ms->allowExtraArgs;
    }

    return ss.str();
}

bool Darkorbit::install(uintptr_t main_app_address)
{
    mouse_click(0, 0, 1); // Mouse click to initialize the click param for flash IPC

    m_main            = memory::read<avm::ScriptObject *>(main_app_address + 0x540);
    m_gc              = avm::get_block_header(m_main)->gc;
    m_tooltips        = TooltipFix{}; // a new game: resolve the TooltipControl again
    m_ships           = ShipFix{};
    m_screen_manager  = m_main->get_at<avm::ScriptObject *>(0x1f8);
    m_gui_manager     = m_main->get_at<avm::ScriptObject *>(0x200);
    m_event_manager   = m_screen_manager->get_at<avm::ScriptObject *>(0xc8);

    utils::log("[+] Main {x}\n", m_main);
    utils::log("[+] Screen {x}\n", m_screen_manager);
    utils::log("[+] Event {x}\n", m_event_manager);

    auto vtable          = m_main->get_at<uintptr_t>(0x10);
    auto vtable_init     = memory::read<uintptr_t>(vtable + 0x10);
    auto vtable_scope    = memory::read<uintptr_t>(vtable_init + 0x18);
    abc_env              = memory::read<avm::AbcEnv *>(vtable_scope + 0x10);
    m_const_pool         = abc_env->pool;


    avm::Multiname *proxy_mn = abc_env->pool->find_multiname("ItemsControlMenuProxy");

    avm::ScriptObject *menu_proxy_obj = flash_stuff::finddef(m_main->vtable->einit, proxy_mn); //  abc_env->finddef("ItemsControlMenuProxy$")

    if (!menu_proxy_obj)
    {
        utils::log("[!] Failed to find menu proxy global\n");
        return false;
    }
    else if (auto *menu_proxy = menu_proxy_obj->get_at<avm::ClassClosure *>(0x20))
    {
        avm::ScriptObject *proxy_object = menu_proxy->construct();
        avm::MethodEnv *send_action     = proxy_object->vtable->get_methods().at(36);
        uint32_t packet_mn              = send_action->method_info->get_params()[0];
        avm::Multiname *mn              = m_const_pool->get_multiname(packet_mn);
        avm::ScriptObject *global       = flash_stuff::finddef(send_action, mn);

        m_action_closure = global->get_at<avm::ClassClosure *>(0x20);
    }
    else
    {
        utils::log("[!] Failed to find ItemsControlMenuProxy closure!!\n");
        return false;
    }

    if (auto timer_method = m_screen_manager->vtable->methods[34]->method_info)
    {
        using namespace std::placeholders;
        utils::log("[+] Found gui timer method at {x}\n", reinterpret_cast<uintptr_t>(timer_method));
        hook_flash_function(timer_method, std::bind(&Darkorbit::handle_async_calls, this, _1, _2, _3));
    }
    else
    {
        utils::log("[!] Failed to find GuiManager timer!!\n");
        return false;
    }

    if (!m_ipc.Running())
    {
        if (m_ipc.Init())
            m_ipc.Run([this](flash_ipc::Message &msg) { return handle_ipc_message(msg); });
        else
            utils::log("[!] Failed to init ipc\n");
    }

    m_installed = true;
    m_ipc.Heartbeat();
    m_ipc.SetInstalled(true);
    utils::log("[+] Installed\n");
    return true;
}

bool Darkorbit::uninstall()
{
    utils::log("[-] Uninstalling...\n");

    m_installed = false;
    m_ipc.SetInstalled(false);

    for (auto &[id, hook] : m_hooks)
    {
        if (!hook.removed)
            hook.restore();
        hook.removed = true;
    }

    // A hook_proxy may be on the stack (uninstall triggered from inside a hooked call),
    // erase the entries once it has returned.
    if (m_proxy_depth > 0)
        m_clear_hooks_pending = true;
    else
        m_hooks.clear();

    m_refine_multiname = 0;
    m_item_prop_mn = 0;

    // Drop queued tasks: waiting ipc requests are released immediately (broken_promise).
    // The ipc thread itself keeps running and answers NOT_READY until the next install.
    {
        std::scoped_lock lk { m_call_mut };
        m_async_calls.clear();
    }
    return true;
}
