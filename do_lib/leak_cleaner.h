#ifndef LEAK_CLEANER_H
#define LEAK_CLEANER_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "avm.h"
#include "game_layout.h"

class Darkorbit;

// Works around memory leaks of the game client: objects the game stopped using but left
// registered on something long-lived (a singleton, a static signal, a shared geometry, material or
// animation node, ...), so they and everything they reference stay alive until the map is left or
// the game is reloaded. Everything runs on the game thread and releases them with the game's own
// methods, at the moment the game throws them away.
//
// Each leak has its own part, named in the logs:
//   [cleaner] ready: <parts>
//   [cleaner: <part>] <what it released in the last minute> (<total> in total)
//   [cleaner: <part>] disabled: <reason>      the game changed, the part stays off
class LeakCleaner
{
public:
    explicit LeakCleaner(Darkorbit &game) : m_game(game) { reset(); }

    // a new game (install): forget everything, resolve again on the next tick
    void reset();

    // game thread, from the GUI timer hook. Calls into the VM: a collection may uninstall us
    // meanwhile (free_chunk), the caller must check that afterwards.
    void tick();

private:
    enum Part
    {
        TOOLTIPS, SHIPS, TWEENS, VIEWS, MATERIALS, MARKERS, LIGHTS, ROCKETS, ROBOTS, CANNONS, MODELS, PART_COUNT
    };

    struct Stats
    {
        const char *name;
        const char *verb;       // "<verb> <n> <what>"
        const char *what;
        bool ready = false;
        bool disabled = false;
        uint64_t pending = 0;   // released since the last log
        uint64_t total = 0;
    };

    static const Stats DEFAULT_STATS[PART_COUNT];
    Stats m_stats[PART_COUNT];
    bool m_resolved = false;
    GameLayout m_layout;              // the game's classes and members, by name or structure
    bool m_layout_resolved = false;
    uint64_t m_last_prune = 0;
    uint64_t m_last_log = 0;

    bool installed() const;
    bool active(Part part) const { return !m_stats[part].disabled && installed(); }
    bool ours(avm::ScriptObject *obj) const;   // an object of the game's GC
    bool disable(Part part, const char *why);  // logs the reason, always false
    bool ready(Part part);                     // always true
    void count(Part part, uint64_t n);
    void log_released(uint64_t now);

    using HookFn = std::function<void(avm::ScriptObject *self, uint32_t argc, uintptr_t *argv)>;

    // the hookable MethodEnv of a method in this vtable (a class' for statics, the instance
    // vtable otherwise): nullptr and *why if not found or already hooked
    avm::MethodEnv *hookable(avm::VTable *vtable, const char *method, const char **why);
    // the class object / instance vtable of a game class (see m_layout): nullptr and *why if not found
    avm::ScriptObject *class_object(const char *class_name, const char **why);
    avm::VTable *instance_vtable(const char *class_name, const char **why);
    // run fn(this) right before every call of env (the original runs afterwards)
    void hook_before(avm::MethodEnv *env, HookFn fn);
    // hook_before() a method of each class (whose instances declare these fields): the classes not
    // as expected are logged and left out, the part is disabled if none is left
    bool hook_classes(Part part, std::initializer_list<std::pair<const char *, const char *>> class_methods,
                      std::initializer_list<const char *> fields, const HookFn &fn);

    // Value of / write to a var or const obj's class (or a base class) declares, by name
    Atom read_slot(avm::ScriptObject *obj, const char *name);
    bool write_slot(avm::ScriptObject *obj, const char *name, Atom value);
    avm::Multiname *slot_name(avm::ScriptObject *obj, const char *name);
    // vtable index of a method of obj's class (0: none)
    uint32_t method_of(avm::ScriptObject *obj, const char *name);
    // obj's class is exactly this one
    bool is_class(avm::ScriptObject *obj, const char *class_name);
    // elements of a Vector.<SomeClass>
    std::vector<avm::ScriptObject *> vector_objects(avm::ScriptObject *vec);

    // The lookups above run for every object the hooks see. Cached by VM pointer (Traits or
    // VTable) and name: a string literal, its address is the key. Dropped whenever the VM frees
    // memory (avm::cache_generation), like the lib's other VM pointer caches.
    using LookupKey = std::pair<const void *, const char *>;
    struct LookupHash
    {
        size_t operator()(const LookupKey &k) const
        {
            return std::hash<const void *>()(k.first) * 31 + std::hash<const void *>()(k.second);
        }
    };
    template <typename T>
    using LookupCache = std::unordered_map<LookupKey, T, LookupHash>;
    LookupCache<avm::Multiname *> m_slot_names;   // nullptr: no such slot
    LookupCache<uint32_t> m_method_indices;       // 0: no such method
    LookupCache<bool> m_class_checks;
    uint32_t m_cache_generation = 0;
    void check_caches();
    // the object values of a flash.utils.Dictionary (its hashtable read directly, checked);
    // false if it isn't one as expected
    bool dictionary_values(avm::ScriptObject *dict, std::vector<avm::ScriptObject *> &out);

    // tooltips: the game never unregisters the tooltips of action bar elements it throws away,
    // the TooltipControl singleton keeps them (and their whole display tree) forever. Every 15 s
    // remove the tooltips of objects that left the stage with the game's own removeToolTip.
    bool resolve_tooltips();
    void prune_tooltips();
    struct
    {
        avm::ScriptObject *control = nullptr;     // TooltipControl._instance (lives in a static)
        avm::Multiname *list_mn = nullptr;        // TooltipControl.toolTipsList
        uint32_t get_target_index = 0;            // vtable index of ToolTipHook.getTargetObject
        avm::Multiname *stage_mn = nullptr;       // public DisplayObject.stage
        uint32_t remove_index = 0;                // vtable index of TooltipControl.removeToolTip
    } m_tooltips;

    // ships: ShipUpgradeDisplay (upgrade icons in a ship's HUD) subscribes to the map's
    // lockManager.targetChanged and never unsubscribes: every ship that dies stays alive with its
    // HUD, traits and 3D model until the map is left. Every 15 s remove the listeners of displays
    // whose ship left the map with the game's own Slot.remove().
    bool resolve_ships();
    void prune_ship_listeners();
    struct
    {
        avm::ScriptObject *screen = nullptr;      // ScreenManager (checked)
        uint32_t handler_index = 0;               // vtable index of ShipUpgradeDisplay.handleLockChanged
        uint32_t remove_index = 0;                // vtable index of Slot.remove
    } m_ships;

    // tweens: GreenSock 11 (bundled with the game) takes a finished tween out of its timeline's
    // list but leaves the tween's own nextNode/prevNode pointing at its old neighbours. Whatever
    // keeps one finished tween keeps those alive, they keep theirs, ... and each tween its
    // onComplete closure: one idle ship's MoveTrait tween holds a chain of dead ships until the map
    // is left. Right before TweenLite.collectRetiredTweens (after the frame's rendering, the only
    // time GreenSock walks old links) clear the links of the finished tweens in masterList and of
    // the finished tweens they lead to.
    bool resolve_tweens();
    void unlink_retired_tweens();
    avm::ScriptObject *m_tween_lite = nullptr;    // TweenLite$ (class object: the statics)

    // views: away3d (bundled with the game) PrefabView3D.playAnimation() listens for
    // PLAYBACK_COMPLETE on the node animator.play() returns. That node belongs to the model's
    // animation set, which every view of the model shares, and the listener only goes away when
    // the animation completes: never for the looping idle animation. Every ship view ever shown
    // stays alive (meshes, materials) while its model is loaded. Before PrefabView3D.dispose()
    // remove the view's listener from the nodes of its animators' animation sets.
    bool resolve_views();
    void release_view_listeners(avm::ScriptObject *view);
    struct
    {
        avm::Multiname *handler_mn = nullptr;     // PrefabView3D.handleAnimPlaybackComplete (private)
        avm::ScriptObject *event_class = nullptr; // AnimationStateEvent$ (PLAYBACK_COMPLETE)
    } m_views;

    // materials: PrefabMaterialManager (one per 3D model in use) builds a prototype material only
    // to ask its texture types. The material subscribes to three static Settings3D signals, and the
    // manager's dispose() drops it without disposing it: every manager ever created leaves a
    // material with its shader passes behind. Dispose the prototype right before
    // PrefabMaterialManager.dispose().
    bool resolve_materials();
    void dispose_material_prototype(avm::ScriptObject *manager);

    // markers: OutOfRangeEffect3D (the markers shown when attacking out of range) makes up to
    // three marker meshes per event on its static shared plane and takes them off the screen
    // after 2 s (removeAndCount) without disposing them: each stays registered with the static
    // geometry and the shared material forever. Dispose the marker right before removeAndCount().
    bool resolve_markers();
    void dispose_marker(avm::ScriptObject *mesh);

    // lights: away3d's MultiPassMaterialBase (EntityXRayMaterial: ships drawn with the x-ray
    // shader) listens to the shared light picker for light changes and dispose() never stops it:
    // every disposed multi-pass material stays alive with its passes. Clear lightPicker (its setter
    // removes the listener) right before MaterialBase.dispose() of a multi-pass material.
    bool resolve_lights();
    void release_light_listener(avm::ScriptObject *material);
    struct
    {
        avm::Traits *multi_pass = nullptr;        // MultiPassMaterialBase (instance traits)
        avm::Multiname *setter_mn = nullptr;      // MultiPassMaterialBase.lightPicker (setter)
    } m_lights;

    // rockets: heat seeking rockets (Rocket3D, BezierRocket3D, TripleBezierRocket3D) carry a marker
    // mesh on their class' static sphere geometry and static material. disposeThis() only takes it
    // off the rocket: every marker ever shown stays registered with the geometry and the material.
    // Dispose the marker right before disposeThis().
    bool resolve_rockets();
    void dispose_rocket_marker(avm::ScriptObject *rocket);

    // robots: RepairRobotEffect3D clones the sparks particle group of the cached particle asset
    // (one clone as a template, one per robot) and cleanup() never disposes the clones: they share
    // geometries and materials with the cached asset (kept for the whole map), so every repair
    // robot ever shown stays alive with its sparks. Dispose the clones right before cleanup().
    bool resolve_robots();
    void dispose_robot_sparks(avm::ScriptObject *effect);

    // cannons: CannonAttackHandler plays a CannonShotEffect for every cannon shot, typed
    // PERSISTANT but only played (never added to or removed from the EffectManager). Persistent,
    // it registers on the attacker's entityDeactivated signal and EffectsViewManager3D keeps its
    // view (ChainboltEffect3D) until the map is left: every shot ever fired stays alive with the
    // attacking ship. Before EffectManager.playEffect() make it NOT_PERSISTANT, what it is: its
    // view still plays and disposes itself after 1.5 s.
    bool resolve_cannons();
    void make_cannon_shot_one_shot(avm::ScriptObject *effect);
    avm::Traits *m_cannon_shot = nullptr;           // CannonShotEffect (instance traits)

    // models: GeometryLoader and TextureLoader count the uses of every loaded model and texture in
    // a UsageGroup: a weak-keyed Dictionary whose value, the UsageCounter, keeps its key (_target),
    // so no entry ever goes. A model or texture nobody uses any more is disposed and forgotten by
    // its loader, but its counter keeps it (a model: its Loader3D, parsers, AWD blocks, meshes,
    // materials) until the map is left. Right before disposeGeometry() / disposeTexture() disposes
    // one, point its counter's target at the counter itself: the weak key goes at the next
    // collection, with the entry.
    bool resolve_models();
    void release_use_counter(avm::ScriptObject *loader, avm::ScriptObject *content);

    Darkorbit &m_game;
};

#endif // LEAK_CLEANER_H
