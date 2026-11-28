#ifndef GAME_LAYOUT_H
#define GAME_LAYOUT_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "avm.h"

// Finds the game classes and members the lib uses. By name while the client has its names; where
// a name is missing (an obfuscated client) by structure: a fingerprint of the class that declares
// the member, and the member's position among that class' own members of its kind.
//
// The fingerprint covers a class' own fields (their types), its methods and accessors (their
// parameter and return types) and its base classes. Only the names of Flash's own types count,
// every other type is "?": renaming the game's classes and members leaves it unchanged, changing
// the class doesn't. The fingerprints live in the user's spec table (see Spec). Each start with
// names also finds everything by structure and compares: TANOS_DEBUG=1 logs this client's table
// when they differ (the client changed those classes).
class GameLayout
{
public:
    enum Kind : uint8_t { CLASS, FIELD, METHOD, GETTER, SETTER };

    struct Spec
    {
        const char *cls;        // the class; for a member, the class to look in (it or a base declares it)
        const char *member;     // nullptr for a class
        Kind kind;
        bool statics;           // a static member (of the class object); a class: found by its statics
                                // (one without instance members has the structure of many others)
        uint64_t key;           // fingerprint of the declaring class (a class: its own)
        uint16_t position;      // index among the declaring class' own members of this kind
    };

    struct Binding
    {
        avm::Traits *owner = nullptr;       // traits of the declaring class (a class: its instance traits)
        avm::Multiname *name = nullptr;     // a member's exact name
        avm::MethodInfo *method = nullptr;  // methods and accessors
    };

    // Finds every spec: by name (unless by_structure), else by structure. Logs the outcome, and an
    // updated spec table if this client's structure differs from the specs'.
    void resolve(avm::PoolObject *pool, avm::ScriptObject *main, const Spec *specs, size_t count, bool by_structure);
    void clear();

    // instance traits of a class spec (empty if not found)
    const std::vector<avm::Traits *> &classes(const char *name) const;
    // the binding of a member spec that traits or one of its base classes declares, nullptr if none
    const Binding *member(avm::Traits *traits, const char *name, Kind kind) const;
    // the class object of instance traits (nullptr if not found)
    avm::ScriptObject *class_object(avm::Traits *itraits);
    // the name is one of the specs (a game name), not one of Flash's own
    bool known(const char *name, Kind kind) const;
    // the exact name of the position-th field the class of traits declares itself (nullptr if
    // none): for objects reached through a member, whose class is too plain to find by structure
    avm::Multiname *own_field(avm::Traits *traits, uint16_t position);

private:
    struct Found
    {
        std::vector<Binding> bindings;
        uint64_t key = 0;
        uint16_t position = 0;
        bool by_name = false;
    };

    void enumerate();
    uint64_t fingerprint(avm::Traits *traits, int depth = 0);
    uint64_t mix_signature(uint64_t h, avm::PoolObject *pool, uint32_t method_index);
    std::string type_token(avm::PoolObject *pool, uint32_t multiname_index) const;
    bool find_member(avm::Traits *start, const Spec &spec, Binding &out, uint16_t &position);
    bool member_at(avm::Traits *owner, const Spec &spec, Binding &out);
    std::vector<avm::Traits *> by_name(const Spec &spec, bool statics);
    avm::Traits *instance_of(avm::Traits *class_traits);
    avm::Traits *class_of(avm::Traits *instance);
    std::vector<avm::Traits *> by_key(uint64_t key, bool statics);

    avm::PoolObject *m_pool = nullptr;
    bool m_debug = false;                                                  // TANOS_DEBUG: log the table
    avm::ScriptObject *m_main = nullptr;
    std::vector<avm::Traits *> m_traits;                                   // the pool's classes
    std::unordered_map<std::string, std::vector<avm::Traits *>> m_names;   // name -> traits
    std::unordered_map<avm::Traits *, uint64_t> m_fingerprints;
    std::unordered_map<uint64_t, std::vector<avm::Traits *>> m_keys;       // fingerprint -> traits

    std::unordered_map<std::string, std::vector<avm::Traits *>> m_classes;
    std::unordered_map<std::string, std::vector<Binding>> m_members[SETTER + 1];
    std::unordered_set<std::string> m_known[SETTER + 1];
    std::unordered_map<avm::Traits *, avm::ScriptObject *> m_class_objects;
};

#endif // GAME_LAYOUT_H
