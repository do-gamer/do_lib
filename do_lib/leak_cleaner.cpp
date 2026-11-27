#include "leak_cleaner.h"

#include <algorithm>
#include <cstdlib>
#include <unordered_set>

#include "darkorbit.h"
#include "memory.h"
#include "offsets.h"

const LeakCleaner::Stats LeakCleaner::DEFAULT_STATS[PART_COUNT] = {
    { "tooltips",  "removed",  "tooltips of objects off the stage" },
    { "ships",     "released", "dead ships kept by their HUD's lock listener" },
    { "tweens",    "unlinked", "finished tweens" },
    { "views",     "released", "disposed ship views from shared animations" },
    { "materials", "disposed", "prototype materials of freed managers" },
    { "markers",   "disposed", "out of range markers" },
    { "lights",    "released", "disposed x-ray materials from the light picker" },
    { "rockets",   "disposed", "heat seeking rocket markers" },
    { "robots",    "disposed", "repair robot sparks" },
    { "cannons",   "released", "cannon shot effects (made one-shot)" },
    { "models",    "released", "disposed 3D models and textures from their use counters" },
};

static inline bool is_object_atom(Atom a) { return (a & 7) == 1 && (a & ~Atom(7)) != 0; }
static constexpr Atom NULL_ATOM = 1;  // kObjectType without a pointer
static constexpr Atom int_atom(int32_t v) { return (Atom(intptr_t(v)) << 3) | 6; }  // kIntptrType

static inline avm::ScriptObject *as_object(Atom a)
{
    return is_object_atom(a) ? reinterpret_cast<avm::ScriptObject *>(avm::remove_kind(a)) : nullptr;
}

static inline avm::ScriptObject *this_object(uintptr_t *argv)
{
    return avm::remove_kind(reinterpret_cast<avm::ScriptObject *>(argv[0]));
}

// vtable index of a method of Flash's own classes by name (0: none). Calling methods instead
// of reading properties: a wrong name can't make the VM throw through our frames.
static uint32_t vtable_method_index(avm::VTable *vtable, const std::string &name)
{
    auto methods = vtable->get_methods();
    for (uint32_t i = 0; i < methods.size(); i++)
    {
        if (methods[i] && methods[i]->method_info && methods[i]->method_info->name() == name)
            return i;
    }
    return 0;
}

// vtable index of a method a class declares: found in the declaring class' own vtable (base
// of this one), subclasses keep the index (an override takes the same slot). 0: none.
static uint32_t declared_method_index(avm::VTable *vtable, const GameLayout::Binding &method)
{
    for (int depth = 0; vtable && depth < 32; vtable = vtable->base, depth++)
    {
        if (vtable->traits != method.owner)
            continue;
        auto methods = vtable->get_methods();
        for (uint32_t i = 0; i < methods.size(); i++)
        {
            if (methods[i] && methods[i]->method_info == method.method)
                return i;
        }
        return 0;
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

// What the cleaner uses from the game: found by name, or by structure where the client lacks the
// names (key: fingerprint of the declaring class, position: among its own members of the kind;
// see game_layout.h). Each start with names checks the keys against the client; when a client
// update changed these classes, run once with TANOS_DEBUG=1 and paste the table it logs here.
static const GameLayout::Spec SPECS[] = {
    { "TooltipControl", nullptr, GameLayout::CLASS, false, 0x350181e4aa48e2c2, 0 },
    { "TooltipControl", "getInstance", GameLayout::METHOD, true, 0x5a278a8946349fd1, 1 },
    { "TooltipControl", "toolTipsList", GameLayout::FIELD, false, 0x350181e4aa48e2c2, 0 },
    { "TooltipControl", "removeToolTip", GameLayout::METHOD, false, 0x350181e4aa48e2c2, 5 },
    { "ToolTipHook", nullptr, GameLayout::CLASS, false, 0xc58eff1d2de7d2d9, 0 },
    { "ToolTipHook", "getTargetObject", GameLayout::METHOD, false, 0xc58eff1d2de7d2d9, 9 },
    { "ScreenManager", nullptr, GameLayout::CLASS, false, 0xf6b0d1fc950a1848, 0 },
    { "ScreenManager", "_map", GameLayout::FIELD, false, 0xf6b0d1fc950a1848, 35 },
    { "Map", nullptr, GameLayout::CLASS, false, 0xd2d589e467c41eb6, 0 },
    { "Map", "_lockManager", GameLayout::FIELD, false, 0xd2d589e467c41eb6, 5 },
    { "LockManager", nullptr, GameLayout::CLASS, false, 0x992b016e638a96bb, 0 },
    { "LockManager", "targetChanged", GameLayout::FIELD, false, 0x992b016e638a96bb, 0 },
    { "Signal", nullptr, GameLayout::CLASS, false, 0x615dc9f18a7f2987, 0 },
    { "Signal", "slots", GameLayout::FIELD, false, 0xb02d16beb65ac202, 1 },
    { "SlotList", nullptr, GameLayout::CLASS, false, 0xc0b99341cc86adc1, 0 },
    { "SlotList", "nonEmpty", GameLayout::FIELD, false, 0xc0b99341cc86adc1, 2 },
    { "SlotList", "head", GameLayout::FIELD, false, 0xc0b99341cc86adc1, 0 },
    { "SlotList", "tail", GameLayout::FIELD, false, 0xc0b99341cc86adc1, 1 },
    { "Slot", nullptr, GameLayout::CLASS, false, 0xd1945ddc0a021e0b, 0 },
    { "Slot", "_listener", GameLayout::FIELD, false, 0xd1945ddc0a021e0b, 2 },
    { "Slot", "remove", GameLayout::METHOD, false, 0xd1945ddc0a021e0b, 4 },
    { "ShipUpgradeDisplay", nullptr, GameLayout::CLASS, false, 0x94aac8724b3792d1, 0 },
    { "ShipUpgradeDisplay", "_trait", GameLayout::FIELD, false, 0x94aac8724b3792d1, 2 },
    { "ShipUpgradeDisplay", "handleLockChanged", GameLayout::METHOD, false, 0x94aac8724b3792d1, 0 },
    { "ShipUpgradesTrait", "_owner", GameLayout::FIELD, false, 0x395e0720e439ad64, 0 },
    { "MapEntity", "_container", GameLayout::FIELD, false, 0x6afc1cfa32c3a2b8, 5 },
    { "TweenLite", nullptr, GameLayout::CLASS, false, 0x5233e12011cd2e1a, 0 },
    { "TweenLite", "version", GameLayout::FIELD, true, 0x3f967a4963aed823, 0 },
    { "TweenLite", "masterList", GameLayout::FIELD, true, 0x3f967a4963aed823, 10 },
    { "TweenLite", "collectRetiredTweens", GameLayout::METHOD, true, 0x3f967a4963aed823, 5 },
    { "TweenLite", "gc", GameLayout::FIELD, false, 0x7739d24d41943d67, 6 },
    { "TweenLite", "timeline", GameLayout::FIELD, false, 0x7739d24d41943d67, 8 },
    { "TweenLite", "nextNode", GameLayout::FIELD, false, 0x7739d24d41943d67, 16 },
    { "TweenLite", "prevNode", GameLayout::FIELD, false, 0x7739d24d41943d67, 17 },
    { "PrefabView3D", nullptr, GameLayout::CLASS, false, 0x56afdaebbdc3c76c, 0 },
    { "PrefabView3D", "dispose", GameLayout::METHOD, false, 0x56afdaebbdc3c76c, 0 },
    { "PrefabView3D", "handleAnimPlaybackComplete", GameLayout::METHOD, false, 0x56afdaebbdc3c76c, 12 },
    { "PrefabView3D", "_animators", GameLayout::FIELD, false, 0x56afdaebbdc3c76c, 6 },
    { "AnimatorBase", "_animationSet", GameLayout::FIELD, false, 0x76a8a36dacb93ad3, 7 },
    { "AnimationSetBase", "_animations", GameLayout::FIELD, false, 0x3778ac3d97145565, 1 },
    { "AnimationStateEvent", nullptr, GameLayout::CLASS, false, 0xe982342080dce893, 0 },
    { "AnimationStateEvent", "PLAYBACK_COMPLETE", GameLayout::FIELD, true, 0x479419c8e33ed7a8, 0 },
    { "PrefabMaterialManager", nullptr, GameLayout::CLASS, false, 0xaa401095bc9e93c4, 0 },
    { "PrefabMaterialManager", "dispose", GameLayout::METHOD, false, 0xaa401095bc9e93c4, 3 },
    { "PrefabMaterialManager", "_materialPrototype", GameLayout::FIELD, false, 0xaa401095bc9e93c4, 4 },
    { "PrefabMaterialManager", "_disposed", GameLayout::FIELD, false, 0xaa401095bc9e93c4, 6 },
    { "MaterialBase", "dispose", GameLayout::METHOD, false, 0xe80851e68f831e7b, 0 },
    { "OutOfRangeEffect3D", nullptr, GameLayout::CLASS, false, 0xbc16627a589fd537, 0 },
    { "OutOfRangeEffect3D", "removeAndCount", GameLayout::METHOD, false, 0xbc16627a589fd537, 2 },
    { "Mesh", nullptr, GameLayout::CLASS, false, 0x8bd39542e0daadf7, 0 },
    { "Mesh", "dispose", GameLayout::METHOD, false, 0x8bd39542e0daadf7, 3 },
    { "MultiPassMaterialBase", nullptr, GameLayout::CLASS, false, 0x466d34dc533eb4ec, 0 },
    { "MultiPassMaterialBase", "lightPicker", GameLayout::SETTER, false, 0x466d34dc533eb4ec, 6 },
    { "MultiPassMaterialBase", "_lightPicker", GameLayout::FIELD, false, 0xe80851e68f831e7b, 17 },
    { "Rocket3D", nullptr, GameLayout::CLASS, false, 0x85ab94322ff54450, 0 },
    { "Rocket3D", "disposeThis", GameLayout::METHOD, false, 0x85ab94322ff54450, 3 },
    { "Rocket3D", "_heatSeekingMesh", GameLayout::FIELD, false, 0x85ab94322ff54450, 18 },
    { "BezierRocket3D", nullptr, GameLayout::CLASS, false, 0x98394d81e78c3564, 0 },
    { "BezierRocket3D", "disposeThis", GameLayout::METHOD, false, 0x98394d81e78c3564, 9 },
    { "BezierRocket3D", "_heatSeekingMesh", GameLayout::FIELD, false, 0x98394d81e78c3564, 23 },
    { "TripleBezierRocket3D", nullptr, GameLayout::CLASS, false, 0x98394d81e78c3564, 0 },
    { "TripleBezierRocket3D", "disposeThis", GameLayout::METHOD, false, 0x98394d81e78c3564, 9 },
    { "TripleBezierRocket3D", "_heatSeekingMesh", GameLayout::FIELD, false, 0x98394d81e78c3564, 23 },
    { "RepairRobotEffect3D", nullptr, GameLayout::CLASS, false, 0x32148366ece24fb3, 0 },
    { "RepairRobotEffect3D", "cleanup", GameLayout::METHOD, false, 0x32148366ece24fb3, 4 },
    { "RepairRobotEffect3D", "_robots", GameLayout::FIELD, false, 0x32148366ece24fb3, 2 },
    { "RepairRobotEffect3D", "_sparks", GameLayout::FIELD, false, 0x32148366ece24fb3, 3 },
    { "RobotBehavior3D", "_sparks", GameLayout::FIELD, false, 0x4f26922a539bfa83, 0 },
    { "ParticleGroup", nullptr, GameLayout::CLASS, false, 0xc6d51df9fa24fd7b, 0 },
    { "ParticleGroup", "dispose", GameLayout::METHOD, false, 0xc6d51df9fa24fd7b, 2 },
    { "CannonShotEffect", nullptr, GameLayout::CLASS, false, 0x647177114a2098d5, 0 },
    { "CannonShotEffect", "_type", GameLayout::FIELD, false, 0xfd59de64edab46cb, 2 },
    { "EffectManager", nullptr, GameLayout::CLASS, false, 0xf8d8831a195bea49, 0 },
    { "EffectManager", "playEffect", GameLayout::METHOD, false, 0xf8d8831a195bea49, 0 },
    { "GeometryLoader", nullptr, GameLayout::CLASS, false, 0xe2af813a88696d98, 0 },
    { "GeometryLoader", "disposeGeometry", GameLayout::METHOD, false, 0xe2af813a88696d98, 5 },
    { "GeometryLoader", "_loaders", GameLayout::FIELD, false, 0x9808d44bc6fae188, 0 },
    { "GeometryLoader", "_pool", GameLayout::FIELD, false, 0x9808d44bc6fae188, 1 },
    { "TextureLoader", nullptr, GameLayout::CLASS, false, 0x7f9f59c9e47ea114, 0 },
    { "TextureLoader", "disposeTexture", GameLayout::METHOD, false, 0x7f9f59c9e47ea114, 6 },
    { "TextureLoader", "_loaders", GameLayout::FIELD, false, 0x9808d44bc6fae188, 0 },
    { "TextureLoader", "_pool", GameLayout::FIELD, false, 0x9808d44bc6fae188, 1 },
    { "SingleGeometryLoader", "_content", GameLayout::FIELD, false, 0xe6722edcccc410d8, 4 },
    { "SingleGeometryLoader", "lifetime", GameLayout::FIELD, false, 0xe6722edcccc410d8, 1 },
    { "SingleTextureLoader", "_content", GameLayout::FIELD, false, 0x7da28929477c9a97, 4 },
    { "SingleTextureLoader", "lifetime", GameLayout::FIELD, false, 0x7da28929477c9a97, 1 },
    { "UsageGroup", nullptr, GameLayout::CLASS, false, 0x685f203544dbce43, 0 },
    { "UsageGroup", "_usage", GameLayout::FIELD, false, 0x685f203544dbce43, 3 },
    { "UsageCounter", nullptr, GameLayout::CLASS, false, 0x1dab76a3575f2bda, 0 },
    { "UsageCounter", "_target", GameLayout::FIELD, false, 0x1dab76a3575f2bda, 2 },
};

// ---- common ----

void LeakCleaner::reset()
{
    std::copy(std::begin(DEFAULT_STATS), std::end(DEFAULT_STATS), std::begin(m_stats));
    m_resolved = false;
    m_layout_resolved = false;
    m_layout.clear();
    m_last_prune = 0;
    m_last_log = flash_ipc::now_ms();
    m_slot_names.clear();
    m_method_indices.clear();
    m_class_checks.clear();
    m_cache_generation = avm::cache_generation();
    m_tooltips = {};
    m_ships = {};
    m_tween_lite = nullptr;
    m_views = {};
    m_lights = {};
    m_cannon_shot = nullptr;
}

bool LeakCleaner::installed() const
{
    return m_game.m_installed;
}

bool LeakCleaner::ours(avm::ScriptObject *obj) const
{
    return obj && avm::get_block_header(obj)->gc == m_game.m_gc;
}

bool LeakCleaner::disable(Part part, const char *why)
{
    auto &s = m_stats[part];
    if (!s.disabled)
        utils::log("[cleaner: {}] disabled: {}\n", s.name, why);
    s.disabled = true;
    s.ready = false;
    return false;
}

bool LeakCleaner::ready(Part part)
{
    m_stats[part].ready = true;
    return true;
}

void LeakCleaner::count(Part part, uint64_t n)
{
    m_stats[part].pending += n;
    m_stats[part].total += n;
}

void LeakCleaner::log_released(uint64_t now)
{
    if (now - m_last_log < 60000)
        return;
    m_last_log = now;
    bool any = false;
    for (auto &s : m_stats)
    {
        if (!s.pending)
            continue;
        utils::log("[cleaner: {}] {} {} {} ({} in total)\n", s.name, s.verb, s.pending, s.what, s.total);
        s.pending = 0;
        any = true;
    }
    if (any)
        utils::log("[cleaner] GC heap used {} MB\n", flash_stuff::gc_heap_used_mb());
}

void LeakCleaner::tick()
{
    // what the parts use from the game (reads the game's code, never calls into the VM)
    if (!m_layout_resolved)
    {
        m_layout_resolved = true;
        m_layout.resolve(m_game.m_const_pool, m_game.m_main, SPECS, sizeof(SPECS) / sizeof(SPECS[0]),
                         getenv("TANOS_LAYOUT_BY_STRUCTURE") != nullptr);
    }

    // periodic parts: their lists are walked, nothing tells when an entry goes stale
    const uint64_t now = flash_ipc::now_ms();
    if (now - m_last_prune >= 15000)
    {
        m_last_prune = now;
        prune_tooltips();
        if (!installed())
            return;
        prune_ship_listeners();
        if (!installed())
            return;
    }

    // the others hook the game method where the leak starts, once
    if (!m_resolved)
    {
        m_resolved = true;
        for (bool (LeakCleaner::*resolve)() : { &LeakCleaner::resolve_tweens, &LeakCleaner::resolve_views,
                                                &LeakCleaner::resolve_materials, &LeakCleaner::resolve_markers,
                                                &LeakCleaner::resolve_lights, &LeakCleaner::resolve_rockets,
                                                &LeakCleaner::resolve_robots, &LeakCleaner::resolve_cannons,
                                                &LeakCleaner::resolve_models })
        {
            (this->*resolve)();
            if (!installed())
                return;
        }
        std::string parts;
        for (auto &s : m_stats)
        {
            if (s.ready)
                parts += (parts.empty() ? "" : ", ") + std::string(s.name);
        }
        utils::log("[cleaner] ready: {}\n", parts.empty() ? std::string("none") : parts);
    }

    log_released(now);
}

avm::MethodEnv *LeakCleaner::hookable(avm::VTable *vtable, const char *method, const char **why)
{
    const GameLayout::Binding *b = vtable ? m_layout.member(vtable->traits, method, GameLayout::METHOD) : nullptr;
    uint32_t index = b ? declared_method_index(vtable, *b) : 0;
    avm::MethodEnv *env = index ? vtable->methods[index] : nullptr;
    if (!env || !env->method_info)
    {
        *why = "method not found";
        return nullptr;
    }
    // hooks are found by method id: never take over another hook's entry
    auto &hooks = m_game.m_hooks;
    auto it = hooks.find(env->method_info->id);
    if (it != hooks.end() && !it->second.removed)
    {
        *why = "method id already hooked";
        return nullptr;
    }
    return env;
}

avm::ScriptObject *LeakCleaner::class_object(const char *class_name, const char **why)
{
    const auto &classes = m_layout.classes(class_name);
    if (classes.size() != 1)
    {
        *why = classes.empty() ? "class not found" : "class not unique";
        return nullptr;
    }
    avm::ScriptObject *cls = m_layout.class_object(classes[0]);
    if (!cls)
        *why = "class object not found";
    return cls;
}

avm::VTable *LeakCleaner::instance_vtable(const char *class_name, const char **why)
{
    avm::ScriptObject *cls = class_object(class_name, why);
    return cls ? cls->vtable->ivtable : nullptr;
}

void LeakCleaner::hook_before(avm::MethodEnv *env, HookFn fn)
{
    m_game.hook_flash_function(env, [fn = std::move(fn)](avm::MethodEnv *, uint32_t argc, uintptr_t *argv)
    {
        fn(this_object(argv), argc, argv);
    });
}

bool LeakCleaner::hook_classes(Part part, std::initializer_list<std::pair<const char *, const char *>> class_methods,
                               std::initializer_list<const char *> fields, const HookFn &fn)
{
    std::string skipped;
    bool any = false;
    std::unordered_set<int32_t> hooked;   // classes of the same structure come with each of their names
    for (auto [name, method] : class_methods)
    {
        const auto &classes = m_layout.classes(name);
        if (classes.empty())
            skipped += utils::format("{}{}: class not found", skipped.empty() ? "" : ", ", name);
        for (avm::Traits *traits : classes)
        {
            const char *why = nullptr;
            avm::ScriptObject *cls = m_layout.class_object(traits);
            avm::VTable *ivtable = cls ? cls->vtable->ivtable : nullptr;
            avm::MethodEnv *env = nullptr;
            if (!ivtable)
                why = "class object not found";
            else if (!std::all_of(fields.begin(), fields.end(), [this, ivtable](const char *field)
                                  { return m_layout.member(ivtable->traits, field, GameLayout::FIELD) != nullptr; }))
                why = "fields not found";
            else if (const GameLayout::Binding *b = m_layout.member(ivtable->traits, method, GameLayout::METHOD))
            {
                const uint32_t index = declared_method_index(ivtable, *b);
                avm::MethodEnv *mine = index ? ivtable->methods[index] : nullptr;
                if (mine && mine->method_info && hooked.count(mine->method_info->id))
                    continue;
                env = hookable(ivtable, method, &why);
            }
            if (!env)
            {
                skipped += utils::format("{}{}.{}: {}", skipped.empty() ? "" : ", ", name, method, why ? why : "not found");
                continue;
            }
            hooked.insert(env->method_info->id);
            hook_before(env, fn);
            any = true;
        }
    }
    if (!any)
        return disable(part, skipped.c_str());
    if (!skipped.empty())
        utils::log("[cleaner: {}] not cleaned: {}\n", m_stats[part].name, skipped);
    return ready(part);
}

void LeakCleaner::check_caches()
{
    const uint32_t generation = avm::cache_generation();
    if (generation == m_cache_generation)
        return;
    m_slot_names.clear();
    m_method_indices.clear();
    m_class_checks.clear();
    m_cache_generation = generation;
}

// Exact multiname of a slot obj's class (or a base class) declares, nullptr if none
avm::Multiname *LeakCleaner::slot_name(avm::ScriptObject *obj, const char *name)
{
    check_caches();
    const LookupKey key { obj->vtable->traits, name };
    auto it = m_slot_names.find(key);
    if (it == m_slot_names.end())
    {
        const GameLayout::Binding *b = m_layout.member(obj->vtable->traits, name, GameLayout::FIELD);
        it = m_slot_names.emplace(key, b ? b->name : nullptr).first;
    }
    return it->second;
}

uint32_t LeakCleaner::method_of(avm::ScriptObject *obj, const char *name)
{
    check_caches();
    const LookupKey key { obj->vtable, name };
    auto it = m_method_indices.find(key);
    if (it == m_method_indices.end())
    {
        uint32_t index = 0;
        if (m_layout.known(name, GameLayout::METHOD))
        {
            if (const GameLayout::Binding *b = m_layout.member(obj->vtable->traits, name, GameLayout::METHOD))
                index = declared_method_index(obj->vtable, *b);
        }
        else
            index = vtable_method_index(obj->vtable, name);   // Flash's own (removeEventListener)
        it = m_method_indices.emplace(key, index).first;
    }
    return it->second;
}

bool LeakCleaner::is_class(avm::ScriptObject *obj, const char *class_name)
{
    check_caches();
    const LookupKey key { obj->vtable->traits, class_name };
    auto it = m_class_checks.find(key);
    if (it == m_class_checks.end())
    {
        bool is = false;
        if (m_layout.known(class_name, GameLayout::CLASS))
        {
            const auto &classes = m_layout.classes(class_name);
            is = std::find(classes.begin(), classes.end(), obj->vtable->traits) != classes.end();
        }
        else
            is = obj->vtable->traits->name() == class_name;   // Flash's own (Array, Dictionary, ...)
        it = m_class_checks.emplace(key, is).first;
    }
    return it->second;
}

// Value of a slot obj's class (or a base class) declares, 0 if it declares none: getproperty of
// a name the object doesn't have would throw through our frames
Atom LeakCleaner::read_slot(avm::ScriptObject *obj, const char *name)
{
    avm::Multiname *mn = slot_name(obj, name);
    return mn ? flash_stuff::get_property(obj, mn) : 0;
}

// setproperty of a slot obj's class (or a base class) declares; false if it declares none
bool LeakCleaner::write_slot(avm::ScriptObject *obj, const char *name, Atom value)
{
    avm::Multiname *mn = slot_name(obj, name);
    if (!mn)
        return false;
    flash_stuff::set_property(obj, mn, value);
    return true;
}

// Elements of a Vector.<SomeClass> (ObjectVectorObject: storage block, u32 length, atoms from the
// block's first item; measured). Empty for anything else.
std::vector<avm::ScriptObject *> LeakCleaner::vector_objects(avm::ScriptObject *vec)
{
    static const char OBJECT_VECTOR[] = "Vector.<object>";   // m_class_checks key
    std::vector<avm::ScriptObject *> out;
    if (!vec)
        return out;
    check_caches();
    const LookupKey key { vec->vtable->traits, OBJECT_VECTOR };
    auto it = m_class_checks.find(key);
    if (it == m_class_checks.end())
    {
        const std::string name = vec->get_name();
        it = m_class_checks.emplace(key, name.rfind("Vector.<", 0) == 0 && name.back() != '$'
                                             && name != "Vector.<int>" && name != "Vector.<uint>"
                                             && name != "Vector.<Number>").first;
    }
    if (!it->second)
        return out;
    const uintptr_t base = reinterpret_cast<uintptr_t>(vec);
    const uintptr_t storage = memory::read<uintptr_t>(base + offsets::vector_storage);
    const uint32_t length = memory::read<uint32_t>(base + offsets::vector_length);
    if (!storage || avm::get_block_header(reinterpret_cast<void *>(storage))->gc != m_game.m_gc
        || offsets::list_first_item + size_t(length) * sizeof(Atom)
               > avm::get_block_header(reinterpret_cast<void *>(storage))->size)
        return out;
    for (uint32_t i = 0; i < length; i++)
    {
        if (avm::ScriptObject *o = as_object(memory::read<Atom>(storage + offsets::list_first_item + i * sizeof(Atom))))
            out.push_back(o);
    }
    return out;
}

// Object values of a Dictionary: HeapHashtable at traits->hashTableOffset, its atoms block (u8 log
// capacity, key/value pairs after the block's first word; empty key 0, deleted key 4; measured)
bool LeakCleaner::dictionary_values(avm::ScriptObject *dict, std::vector<avm::ScriptObject *> &out)
{
    if (!ours(dict) || !is_class(dict, "Dictionary"))
        return false;
    avm::GC *gc = m_game.m_gc;
    const uint32_t table_at = dict->vtable->traits->hashTableOffset;
    const uintptr_t table = table_at ? memory::read<uintptr_t>(reinterpret_cast<uintptr_t>(dict) + table_at) : 0;
    if (!table || avm::get_block_header(reinterpret_cast<void *>(table))->gc != gc)
        return false;
    const uintptr_t block = memory::read<uintptr_t>(table + offsets::hashtable_atoms) & ~uintptr_t(7);
    const uint8_t log_capacity = memory::read<uint8_t>(table + offsets::hashtable_log_capacity);
    const size_t capacity = log_capacity ? size_t(1) << (log_capacity - 1) : 0;
    if (!block || log_capacity > 24 || avm::get_block_header(reinterpret_cast<void *>(block))->gc != gc
        || offsets::hashtable_first_atom + capacity * sizeof(Atom)
               > avm::get_block_header(reinterpret_cast<void *>(block))->size)
        return false;
    const Atom *atoms = reinterpret_cast<const Atom *>(block + offsets::hashtable_first_atom);
    for (size_t i = 0; i + 1 < capacity; i += 2)
    {
        if (atoms[i] == 0 || atoms[i] == 4)
            continue; // empty / deleted key
        if (avm::ScriptObject *value = as_object(atoms[i + 1]))
            out.push_back(value);
    }
    return true;
}

// ---- tooltips ----

bool LeakCleaner::resolve_tooltips()
{
    const char *why = nullptr;
    avm::ScriptObject *cls = class_object("TooltipControl", &why);
    if (!cls)
        return disable(TOOLTIPS, why);

    // the game's own TooltipControl.getInstance() (static: the class object is 'this')
    uint32_t get_instance = method_of(cls, "getInstance");
    if (!get_instance)
        return disable(TOOLTIPS, "TooltipControl.getInstance not found");
    auto *control = avm::remove_kind(reinterpret_cast<avm::ScriptObject *>(cls->call(get_instance)));
    if (!control || !is_class(control, "TooltipControl"))
        return disable(TOOLTIPS, "unexpected TooltipControl instance");

    avm::Multiname *list_mn = slot_name(control, "toolTipsList");
    if (!list_mn)
        return disable(TOOLTIPS, "TooltipControl.toolTipsList not found");
    uint32_t remove_index = method_of(control, "removeToolTip");
    if (!remove_index)
        return disable(TOOLTIPS, "TooltipControl.removeToolTip not found");

    // DisplayObject.stage, read only on hook targets (InteractiveObjects, never anything else)
    avm::Multiname *stage_mn = public_qname(m_game.m_const_pool, "", "stage");
    if (!stage_mn)
        return disable(TOOLTIPS, "public 'stage' name not found");

    m_tooltips.control = control;
    m_tooltips.list_mn = list_mn;
    m_tooltips.stage_mn = stage_mn;
    m_tooltips.remove_index = remove_index;
    return ready(TOOLTIPS);
}

void LeakCleaner::prune_tooltips()
{
    auto &f = m_tooltips;
    if (!active(TOOLTIPS) || !m_game.m_main)
        return;
    if (!f.control && !resolve_tooltips())
        return;

    Atom list_atom = flash_stuff::get_property(f.control, f.list_mn);
    if (!is_object_atom(list_atom))
        return;
    auto *list = reinterpret_cast<avm::Array *>(avm::remove_kind(list_atom));
    if (!is_class(list, "Array") || !list->data
        || avm::get_block_header(list->data)->gc != m_game.m_gc)  // the storage must be a block of our GC
    {
        disable(TOOLTIPS, "toolTipsList isn't an Array");
        return;
    }
    // dense storage: ListData { vtable, uint32 len, ..., Atom entries[] }
    const uint32_t dense_len = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(list->data) + 8);
    const uint32_t listed = std::min(list->size, dense_len);

    // collect first: removeToolTip splices the list. The targets stay referenced by the list
    // until their own removal, so the GC can't free them meanwhile.
    std::vector<avm::ScriptObject *> stale;
    for (uint32_t i = 0; i < listed && stale.size() < 150; i++)
    {
        Atom hook_atom = (*list)[i];
        if (!is_object_atom(hook_atom))
            continue;
        auto *hook = reinterpret_cast<avm::ScriptObject *>(avm::remove_kind(hook_atom));
        if (!is_class(hook, "ToolTipHook"))
            continue; // e.g. a subclass: leave it alone
        if (!f.get_target_index && !(f.get_target_index = method_of(hook, "getTargetObject")))
        {
            disable(TOOLTIPS, "ToolTipHook.getTargetObject not found");
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
        else if (!is_object_atom(stage) || !is_class(as_object(stage), "Stage"))
        {
            disable(TOOLTIPS, "'stage' didn't read a Stage");
            return;
        }
    }

    for (auto *target : stale)
        f.control->call(f.remove_index, target);   // TooltipControl.removeToolTip(target)
    count(TOOLTIPS, stale.size());
}

// ---- ships ----

bool LeakCleaner::resolve_ships()
{
    avm::ScriptObject *screen = m_game.m_screen_manager;
    if (!ours(screen) || !is_class(screen, "ScreenManager"))
        return disable(SHIPS, "unexpected ScreenManager");
    m_ships.screen = screen;
    return ready(SHIPS);
}

void LeakCleaner::prune_ship_listeners()
{
    auto &f = m_ships;
    if (!active(SHIPS) || !m_game.m_main)
        return;
    if (!f.screen && !resolve_ships())
        return;

    // ScreenManager._map._lockManager.targetChanged.slots: the listener list (osflash SlotList)
    avm::ScriptObject *map = as_object(read_slot(f.screen, "_map"));
    if (!map || !is_class(map, "Map"))
        return;
    avm::ScriptObject *lock = as_object(read_slot(map, "_lockManager"));
    if (!lock || !is_class(lock, "LockManager"))
        return;
    avm::ScriptObject *signal = as_object(read_slot(lock, "targetChanged"));
    if (!signal || !is_class(signal, "Signal"))
        return;

    // collect first, remove after: Slot.remove() replaces the list. Until its own removal each
    // collected Slot stays in the signal's list, so the GC can't free it meanwhile.
    std::vector<avm::ScriptObject *> stale;
    uint32_t n = 0;
    for (avm::ScriptObject *node = as_object(read_slot(signal, "slots")); node && n < 20000; n++)
    {
        if (!is_class(node, "SlotList") || read_slot(node, "nonEmpty") != avm::TRUE)
            break; // SlotList.NIL ends the list
        avm::ScriptObject *slot = as_object(read_slot(node, "head"));
        node = as_object(read_slot(node, "tail"));
        if (!slot || !is_class(slot, "Slot") || stale.size() >= 100)
            continue;
        avm::ScriptObject *closure = as_object(read_slot(slot, "_listener"));
        if (!closure || !is_class(closure, "MethodClosure"))
            continue;
        // a bound method: its receiver, and the MethodEnv it calls
        const auto c = reinterpret_cast<uintptr_t>(closure);
        avm::ScriptObject *display = as_object(*reinterpret_cast<Atom *>(c + offsets::closure_saved_this));
        if (!display || !is_class(display, "ShipUpgradeDisplay"))
            continue; // another listener (lock displays, HealthInfoDisplay, ...)
        if (!f.handler_index && !(f.handler_index = method_of(display, "handleLockChanged")))
        {
            disable(SHIPS, "ShipUpgradeDisplay.handleLockChanged not found");
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

    if (!f.remove_index && !(f.remove_index = method_of(stale.front(), "remove")))
    {
        disable(SHIPS, "Slot.remove not found");
        return;
    }
    for (avm::ScriptObject *slot : stale)
        slot->call(f.remove_index);   // Slot.remove(): targetChanged.remove(listener)
    count(SHIPS, stale.size());
}

// ---- tweens ----

bool LeakCleaner::resolve_tweens()
{
    const char *why = nullptr;
    avm::ScriptObject *cls = class_object("TweenLite", &why);
    if (!cls)
        return disable(TWEENS, why);

    // written for GreenSock 11's lists (SimpleTimeline, TimelineLite, masterList)
    Atom version = read_slot(cls, "version");
    if ((version & 7) != 7 || *reinterpret_cast<const double *>(version & ~Atom(7)) != 11.36)
        return disable(TWEENS, "unexpected GreenSock version");

    avm::MethodEnv *env = hookable(cls->vtable, "collectRetiredTweens", &why);   // static
    if (!env)
        return disable(TWEENS, why);

    m_tween_lite = cls;
    hook_before(env, [this](avm::ScriptObject *, uint32_t, uintptr_t *) { unlink_retired_tweens(); });
    return ready(TWEENS);
}

void LeakCleaner::unlink_retired_tweens()
{
    if (!active(TWEENS) || !m_tween_lite)
        return;

    // TweenLite.masterList: a strong Dictionary, target -> Array of its tweens. Finished tweens
    // stay in it until collectRetiredTweens (the call we run before) takes them out.
    std::vector<avm::ScriptObject *> lists;
    if (!dictionary_values(as_object(read_slot(m_tween_lite, "masterList")), lists))
    {
        disable(TWEENS, "unexpected TweenLite.masterList");
        return;
    }
    std::vector<avm::ScriptObject *> todo;
    for (avm::ScriptObject *value : lists)
    {
        auto *list = reinterpret_cast<avm::Array *>(value);
        if (!is_class(list, "Array") || !list->data || avm::get_block_header(list->data)->gc != m_game.m_gc)
            continue;
        // dense storage: ListData { vtable, uint32 len, ..., Atom entries[] }
        const uint32_t dense_len = *reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(list->data) + 8);
        for (uint32_t j = 0, n = std::min(list->size, dense_len); j < n; j++)
        {
            if (avm::ScriptObject *tween = as_object((*list)[j]))
                todo.push_back(tween);
        }
    }

    // 1) find the finished tweens (gc) out of their timeline's list, then the finished tweens
    // their old links lead to. A tween whose timeline is disabled stays in its list (the
    // timeline links it back when enabled): leave those alone. Nothing changes meanwhile.
    std::vector<avm::ScriptObject *> found;
    std::unordered_set<avm::ScriptObject *> seen;
    while (!todo.empty() && found.size() < 50000)
    {
        avm::ScriptObject *tween = todo.back();
        todo.pop_back();
        if (!seen.insert(tween).second || !ours(tween))
            continue;
        if (read_slot(tween, "gc") != avm::TRUE)
            continue; // in a timeline's list (or not a tween): its links are live
        avm::ScriptObject *timeline = as_object(read_slot(tween, "timeline"));
        if (timeline && read_slot(timeline, "gc") == avm::TRUE)
            continue;
        avm::ScriptObject *next = as_object(read_slot(tween, "nextNode"));
        avm::ScriptObject *prev = as_object(read_slot(tween, "prevNode"));
        if (!next && !prev)
            continue;
        found.push_back(tween);
        if (next)
            todo.push_back(next);
        if (prev)
            todo.push_back(prev);
    }

    // 2) clear their links, last found first: a tween found through another one's link is
    // cleared while that link still keeps it alive (setproperty updates reference counts)
    for (auto it = found.rbegin(); it != found.rend(); ++it)
    {
        write_slot(*it, "nextNode", NULL_ATOM);
        write_slot(*it, "prevNode", NULL_ATOM);
    }
    count(TWEENS, found.size());
}

// ---- views ----

bool LeakCleaner::resolve_views()
{
    const char *why = nullptr;
    avm::VTable *ivtable = instance_vtable("PrefabView3D", &why);   // PrefabView3D and ShipView3D use it
    if (!ivtable)
        return disable(VIEWS, why);
    const GameLayout::Binding *handler = m_layout.member(ivtable->traits, "handleAnimPlaybackComplete", GameLayout::METHOD);
    if (!handler)
        return disable(VIEWS, "PrefabView3D.handleAnimPlaybackComplete not found");
    avm::ScriptObject *events = class_object("AnimationStateEvent", &why);
    if (!events)
        return disable(VIEWS, why);
    if ((read_slot(events, "PLAYBACK_COMPLETE") & 7) != 2)   // kStringType
        return disable(VIEWS, "AnimationStateEvent.PLAYBACK_COMPLETE isn't a String");
    avm::MethodEnv *env = hookable(ivtable, "dispose", &why);
    if (!env)
        return disable(VIEWS, why);

    m_views.handler_mn = handler->name;
    m_views.event_class = events;
    hook_before(env, [this](avm::ScriptObject *view, uint32_t, uintptr_t *) { release_view_listeners(view); });
    return ready(VIEWS);
}

// Runs before PrefabView3D.dispose() (which drops the view's animators)
void LeakCleaner::release_view_listeners(avm::ScriptObject *view)
{
    auto &f = m_views;
    if (!active(VIEWS) || !f.handler_mn || !ours(view))
        return;

    Atom type = read_slot(f.event_class, "PLAYBACK_COMPLETE");
    if ((type & 7) != 2)
        return;
    std::vector<avm::ScriptObject *> nodes;
    for (avm::ScriptObject *animator : vector_objects(as_object(read_slot(view, "_animators"))))
    {
        avm::ScriptObject *set = as_object(read_slot(animator, "_animationSet"));
        if (set)
        {
            for (avm::ScriptObject *node : vector_objects(as_object(read_slot(set, "_animations"))))
                nodes.push_back(node);
        }
    }
    if (nodes.empty())
        return;

    // this.handleAnimPlaybackComplete, as playAnimation() registered it (closures of the same
    // method and object are the same listener)
    Atom handler = flash_stuff::get_property(view, f.handler_mn);
    if (!is_object_atom(handler))
        return;
    for (avm::ScriptObject *node : nodes)
    {
        uint32_t remove = method_of(node, "removeEventListener");
        if (remove)   // EventDispatcher.removeEventListener(type, listener); absent: nothing happens
            node->call(remove, type & ~Atom(7), handler & ~Atom(7));
    }
    count(VIEWS, 1);
}

// ---- materials ----

bool LeakCleaner::resolve_materials()
{
    return hook_classes(MATERIALS, { { "PrefabMaterialManager", "dispose" } }, { "_materialPrototype", "_disposed" },
                        [this](avm::ScriptObject *manager, uint32_t, uintptr_t *)
    {
        dispose_material_prototype(manager);
    });
}

// Runs before PrefabMaterialManager.dispose() (which drops _materialPrototype)
void LeakCleaner::dispose_material_prototype(avm::ScriptObject *manager)
{
    if (!active(MATERIALS) || !ours(manager))
        return;
    if (read_slot(manager, "_disposed") != avm::FALSE)
        return; // disposed before: dispose() returns right away
    avm::ScriptObject *prototype = as_object(read_slot(manager, "_materialPrototype"));
    if (!prototype)
        return;
    uint32_t dispose = method_of(prototype, "dispose");
    if (!dispose)
        return;
    prototype->call(dispose);   // the material's own dispose(): leaves the Settings3D signals
    count(MATERIALS, 1);
}

// ---- markers ----

bool LeakCleaner::resolve_markers()
{
    // private
    return hook_classes(MARKERS, { { "OutOfRangeEffect3D", "removeAndCount" } }, {},
                        [this](avm::ScriptObject *, uint32_t argc, uintptr_t *argv)
    {
        if (argc >= 1)   // removeAndCount(mesh, type)
            dispose_marker(avm::remove_kind(reinterpret_cast<avm::ScriptObject *>(argv[1])));
    });
}

// Runs before OutOfRangeEffect3D.removeAndCount(mesh): Mesh.dispose() takes it off its parent (what
// removeAndCount would do) and unregisters it from the shared geometry and material
void LeakCleaner::dispose_marker(avm::ScriptObject *mesh)
{
    if (!active(MARKERS) || !ours(mesh) || !is_class(mesh, "Mesh"))
        return;
    uint32_t dispose = method_of(mesh, "dispose");
    if (!dispose)
        return;
    mesh->call(dispose);
    count(MARKERS, 1);
}

// ---- lights ----

bool LeakCleaner::resolve_lights()
{
    const char *why = nullptr;
    avm::VTable *ivtable = instance_vtable("MultiPassMaterialBase", &why);
    if (!ivtable)
        return disable(LIGHTS, why);
    const GameLayout::Binding *setter = m_layout.member(ivtable->traits, "lightPicker", GameLayout::SETTER);
    if (!setter || !m_layout.member(ivtable->traits, "_lightPicker", GameLayout::FIELD))
        return disable(LIGHTS, "MultiPassMaterialBase.lightPicker not found");
    // MaterialBase.dispose(), inherited (the entity materials reach it through super.dispose())
    avm::MethodEnv *env = hookable(ivtable, "dispose", &why);
    if (!env)
        return disable(LIGHTS, why);

    m_lights.multi_pass = ivtable->traits;
    m_lights.setter_mn = setter->name;
    hook_before(env, [this](avm::ScriptObject *material, uint32_t, uintptr_t *) { release_light_listener(material); });
    return ready(LIGHTS);
}

// Runs before MaterialBase.dispose() (any material)
void LeakCleaner::release_light_listener(avm::ScriptObject *material)
{
    auto &f = m_lights;
    if (!active(LIGHTS) || !f.setter_mn || !ours(material))
        return;
    bool multi_pass = false;
    avm::Traits *t = material->vtable->traits;
    for (int depth = 0; t && depth < 32 && !multi_pass; t = t->base, depth++)
        multi_pass = t == f.multi_pass;
    if (!multi_pass || !is_object_atom(read_slot(material, "_lightPicker")))
        return;
    // material.lightPicker = null: the (most derived) setter removes its light picker listener
    flash_stuff::set_property(material, f.setter_mn, NULL_ATOM);
    count(LIGHTS, 1);
}

// ---- rockets ----

bool LeakCleaner::resolve_rockets()
{
    // three unrelated classes with the same code
    return hook_classes(ROCKETS, { { "Rocket3D", "disposeThis" }, { "BezierRocket3D", "disposeThis" },
                                   { "TripleBezierRocket3D", "disposeThis" } },
                        { "_heatSeekingMesh" }, [this](avm::ScriptObject *rocket, uint32_t, uintptr_t *)
    {
        dispose_rocket_marker(rocket);
    });
}

// Runs before disposeThis() of a rocket: Mesh.dispose() takes the heat seeking marker off the
// rocket (what disposeThis would do) and unregisters it from the shared geometry and material
void LeakCleaner::dispose_rocket_marker(avm::ScriptObject *rocket)
{
    if (!active(ROCKETS) || !ours(rocket))
        return;
    avm::ScriptObject *mesh = as_object(read_slot(rocket, "_heatSeekingMesh"));
    if (!ours(mesh) || !is_class(mesh, "Mesh"))
        return;
    uint32_t dispose = method_of(mesh, "dispose");
    if (!dispose)
        return;
    mesh->call(dispose);
    count(ROCKETS, 1);
}

// ---- robots ----

bool LeakCleaner::resolve_robots()
{
    // private
    return hook_classes(ROBOTS, { { "RepairRobotEffect3D", "cleanup" } }, { "_robots", "_sparks" },
                        [this](avm::ScriptObject *effect, uint32_t, uintptr_t *)
    {
        dispose_robot_sparks(effect);
    });
}

// Runs before RepairRobotEffect3D.cleanup() (on reload and disposeView): ParticleGroup.dispose(),
// as the game's ObjectsPool does for clones, takes the sparks off the robot and unregisters their
// meshes from the shared geometries and materials. The robots keep their (disposed) _sparks: a
// pending sparksOff() still sets its visibility. Disposing a group twice does nothing.
void LeakCleaner::dispose_robot_sparks(avm::ScriptObject *effect)
{
    if (!active(ROBOTS) || !ours(effect))
        return;

    std::vector<avm::ScriptObject *> groups;
    for (avm::ScriptObject *robot : vector_objects(as_object(read_slot(effect, "_robots"))))
    {
        if (avm::ScriptObject *sparks = as_object(read_slot(robot, "_sparks")))
            groups.push_back(sparks);
    }
    if (avm::ScriptObject *sparks = as_object(read_slot(effect, "_sparks")))   // the template clone
        groups.push_back(sparks);

    for (avm::ScriptObject *group : groups)
    {
        if (!installed())
            return;
        if (!ours(group) || !is_class(group, "ParticleGroup"))
            continue;
        uint32_t dispose = method_of(group, "dispose");
        if (!dispose)
            continue;
        group->call(dispose);
        count(ROBOTS, 1);
    }
}

// ---- cannons ----

bool LeakCleaner::resolve_cannons()
{
    const char *why = nullptr;
    const auto &shots = m_layout.classes("CannonShotEffect");
    if (shots.size() != 1)
        return disable(CANNONS, "CannonShotEffect not found");
    if (!m_layout.member(shots[0], "_type", GameLayout::FIELD))   // Effect._type
        return disable(CANNONS, "Effect._type not found");
    avm::VTable *manager = instance_vtable("EffectManager", &why);
    if (!manager)
        return disable(CANNONS, why);
    avm::MethodEnv *env = hookable(manager, "playEffect", &why);
    if (!env)
        return disable(CANNONS, why);

    m_cannon_shot = shots[0];
    hook_before(env, [this](avm::ScriptObject *, uint32_t argc, uintptr_t *argv)
    {
        if (argc >= 1)   // playEffect(effect)
            make_cannon_shot_one_shot(avm::remove_kind(reinterpret_cast<avm::ScriptObject *>(argv[1])));
    });
    return ready(CANNONS);
}

// Runs before EffectManager.playEffect(effect) (every effect: compares a pointer first)
void LeakCleaner::make_cannon_shot_one_shot(avm::ScriptObject *effect)
{
    if (!effect || !m_cannon_shot || !ours(effect) || effect->vtable->traits != m_cannon_shot || !active(CANNONS))
        return;
    if (read_slot(effect, "_type") != int_atom(1))   // Effect.PERSISTANT
        return;
    write_slot(effect, "_type", int_atom(0));         // Effect.NOT_PERSISTANT
    count(CANNONS, 1);
}

// ---- models ----

bool LeakCleaner::resolve_models()
{
    // the UsageGroup's notUsed listeners (private)
    return hook_classes(MODELS, { { "GeometryLoader", "disposeGeometry" }, { "TextureLoader", "disposeTexture" } },
                        { "_loaders", "_pool" },
                        [this](avm::ScriptObject *loader, uint32_t argc, uintptr_t *argv)
    {
        if (argc >= 1)   // disposeGeometry(content) / disposeTexture(content)
            release_use_counter(loader, avm::remove_kind(reinterpret_cast<avm::ScriptObject *>(argv[1])));
    });
}

// Runs before GeometryLoader.disposeGeometry(content) / TextureLoader.disposeTexture(content), the
// notUsed listener of the loader's UsageGroup. Those dispose the content (and forget its single
// loader) only if its loader lives as long as its use counter (lifetime LIFETIME_USE_COUNTER, 0).
void LeakCleaner::release_use_counter(avm::ScriptObject *loader, avm::ScriptObject *content)
{
    if (!active(MODELS) || !ours(loader) || !ours(content))
        return;

    // the content's single loader (the first one, as disposeX takes): is it disposed now?
    std::vector<avm::ScriptObject *> singles;
    if (!dictionary_values(as_object(read_slot(loader, "_loaders")), singles))
        return;
    bool disposed = false;
    for (avm::ScriptObject *single : singles)
    {
        if (!ours(single) || as_object(read_slot(single, "_content")) != content)
            continue;
        const Atom lifetime = read_slot(single, "lifetime");
        disposed = (lifetime & 7) == 6 && (intptr_t(lifetime) >> 3) <= 0;   // uint, int atom
        break;
    }
    if (!disposed)
        return; // kept (a longer lifetime): its counter is used again

    // its UsageCounter: a value of the group's weak-keyed Dictionary with this target
    avm::ScriptObject *group = as_object(read_slot(loader, "_pool"));
    if (!ours(group) || !is_class(group, "UsageGroup"))
        return;
    std::vector<avm::ScriptObject *> counters;
    if (!dictionary_values(as_object(read_slot(group, "_usage")), counters))
        return;
    for (avm::ScriptObject *counter : counters)
    {
        if (!ours(counter) || !is_class(counter, "UsageCounter")
            || as_object(read_slot(counter, "_target")) != content)
            continue;
        // the counter itself, not null: should the game ever count the disposed content again,
        // notUsed then names no loader's content (null would match a loader still loading)
        write_slot(counter, "_target", reinterpret_cast<Atom>(counter) | 1);
        count(MODELS, 1);
        return;
    }
}
