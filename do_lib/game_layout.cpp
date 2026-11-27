#include "game_layout.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "binary_stream.h"
#include "flash_stuff.h"
#include "utils.h"

namespace
{

constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ull;
constexpr uint64_t FNV_PRIME = 0x100000001b3ull;

uint64_t mix(uint64_t h, const void *data, size_t n)
{
    auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; i++)
    {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

uint64_t mix(uint64_t h, const std::string &s)
{
    return mix(mix(h, s.data(), s.size()), "", 1);
}

uint64_t mix(uint64_t h, uint64_t v)
{
    return mix(h, &v, sizeof(v));
}

// Flash's own top level types: their names stay in any client
const std::set<std::string> TOP_LEVEL_TYPES = {
    "int", "uint", "Number", "Boolean", "String", "Object", "Array", "Function", "Class", "XML",
    "XMLList", "Date", "RegExp", "Error", "Namespace", "QName", "void"
};

const char *const KIND_NAMES[] = { "CLASS", "FIELD", "METHOD", "GETTER", "SETTER" };

// a QName (flag 0x02, no ATTR, RTNS, RTNAME, NSSET), the only multinames with a namespace
bool is_qname(avm::Multiname *mn)
{
    return mn && (mn->flags & 0x1f) == 0x02 && mn->ns;
}

// trait kinds counted together for positions (fields: vars and consts)
GameLayout::Kind group_of(avm::TraitKind kind)
{
    switch (kind)
    {
        case avm::TRAIT_Slot:
        case avm::TRAIT_Const:  return GameLayout::FIELD;
        case avm::TRAIT_Method: return GameLayout::METHOD;
        case avm::TRAIT_Getter: return GameLayout::GETTER;
        case avm::TRAIT_Setter: return GameLayout::SETTER;
        default:                return GameLayout::CLASS;   // nested classes, functions: not members we use
    }
}

std::string spec_name(const GameLayout::Spec &spec)
{
    std::string name = spec.cls;
    if (spec.statics)
        name += "$";
    if (spec.member)
        name += std::string(".") + spec.member;
    return name;
}

} // namespace

void GameLayout::clear()
{
    m_pool = nullptr;
    m_main = nullptr;
    m_traits.clear();
    m_names.clear();
    m_fingerprints.clear();
    m_keys.clear();
    m_classes.clear();
    for (auto &members : m_members)
        members.clear();
    for (auto &known : m_known)
        known.clear();
    m_class_objects.clear();
}

// The pool's classes: the declaring traits of its methods (every class declares at least its
// constructor and static initializer). The method list's length word is masked: walk the entries
// while each one's id is its index, within the list's block.
void GameLayout::enumerate()
{
    std::unordered_set<avm::Traits *> seen;
    avm::MethodInfo **list = m_pool->method_list;
    if (!list)
        return;
    const size_t block = avm::get_block_header(list)->size;
    const size_t capacity = block > 0x10 ? (block - 0x10) / sizeof(void *) : 0;
    for (size_t i = 0; i < capacity && i < (1u << 22); i++)
    {
        avm::MethodInfo *mi = list[i + 2];
        if (!mi || mi->id != int32_t(i))
            break;
        if (!mi->declarer.is_traits())
            continue;
        avm::Traits *traits = mi->declarer.traits();
        if (!traits || traits->pool != m_pool || (traits->pos_type != 0 && traits->pos_type != 1))
            continue;
        if (seen.insert(traits).second)
        {
            m_traits.push_back(traits);
            m_names[traits->name()].push_back(traits);
        }
    }
}

std::string GameLayout::type_token(avm::PoolObject *pool, uint32_t multiname_index) const
{
    if (!multiname_index)
        return "*";
    avm::Multiname *mn = pool->get_multiname(multiname_index);
    if (!is_qname(mn))
        return "?";
    const std::string uri = mn->ns->get_uri();
    const std::string name = mn->get_name();
    if (uri.rfind("flash.", 0) == 0 || uri == "__AS3__.vec")
        return uri + "::" + name;
    if (uri.empty() && TOP_LEVEL_TYPES.count(name))
        return name;
    return "?";
}

uint64_t GameLayout::mix_signature(uint64_t h, avm::PoolObject *pool, uint32_t method_index)
{
    avm::MethodInfo *mi = pool->get_method(method_index);
    if (!mi || !mi->abc_info)
        return mix(h, std::string("?"));
    BinaryStream s { mi->abc_info };
    const uint32_t params = s.read_u32();
    h = mix(h, uint64_t(params));
    h = mix(h, type_token(pool, s.read_u32()));   // return type
    for (uint32_t i = 0; i < params && i < 64; i++)
        h = mix(h, type_token(pool, s.read_u32()));
    return h;
}

uint64_t GameLayout::fingerprint(avm::Traits *traits, int depth)
{
    if (!traits)
        return 0;
    auto it = m_fingerprints.find(traits);
    if (it != m_fingerprints.end())
        return it->second;

    uint64_t h = FNV_OFFSET;
    if (traits->pool != m_pool || depth > 32)
    {
        // Flash's own classes (and other code's): by name, those never change
        h = mix(mix(h, traits->ns ? traits->ns->get_uri() : std::string()), traits->name());
    }
    else
    {
        h = mix(h, uint64_t(traits->pos_type));
        for (auto &trait : traits->parse_traits().traits)
        {
            h = mix(h, uint64_t(trait.kind));
            switch (group_of(trait.kind))
            {
                case FIELD:
                    h = mix(h, type_token(traits->pool, trait.type_id));
                    break;
                case METHOD:
                case GETTER:
                case SETTER:
                    h = mix_signature(h, traits->pool, trait.id);
                    break;
                default:
                    break;
            }
        }
        h = mix(h, fingerprint(traits->base, depth + 1));
        if (traits->pos_type == 1)
            h = mix(h, fingerprint(instance_of(traits), depth + 1));
    }
    m_fingerprints[traits] = h;
    return h;
}

// The instance traits of class traits: the VM names them "Foo" and "Foo$" (also with obfuscated
// names), in the same namespace
avm::Traits *GameLayout::instance_of(avm::Traits *class_traits)
{
    std::string name = class_traits->name();
    if (name.empty() || name.back() != '$')
        return nullptr;
    name.pop_back();
    auto it = m_names.find(name);
    if (it == m_names.end())
        return nullptr;
    avm::Traits *found = nullptr;
    for (avm::Traits *t : it->second)
    {
        if (t->pos_type == 0 && t->ns == class_traits->ns)
        {
            if (found)
                return nullptr;
            found = t;
        }
    }
    return found;
}

// a member spec in start or its base classes, by name
bool GameLayout::find_member(avm::Traits *start, const Spec &spec, Binding &out, uint16_t &position)
{
    for (avm::Traits *t = start; t && t->pool == m_pool; t = t->base)
    {
        uint16_t index = 0;
        for (auto &trait : t->parse_traits().traits)
        {
            if (group_of(trait.kind) != spec.kind)
                continue;
            if (trait.name == spec.member)
            {
                out.owner = t;
                out.name = t->pool->get_multiname(trait.name_index);
                out.method = spec.kind == FIELD ? nullptr : t->pool->get_method(trait.id);
                position = index;
                return out.name != nullptr;
            }
            index++;
        }
    }
    return false;
}

// a member spec by its position in owner
bool GameLayout::member_at(avm::Traits *owner, const Spec &spec, Binding &out)
{
    uint16_t index = 0;
    for (auto &trait : owner->parse_traits().traits)
    {
        if (group_of(trait.kind) != spec.kind)
            continue;
        if (index++ != spec.position)
            continue;
        out.owner = owner;
        out.name = owner->pool->get_multiname(trait.name_index);
        out.method = spec.kind == FIELD ? nullptr : owner->pool->get_method(trait.id);
        return out.name != nullptr;
    }
    return false;
}

// the class traits ("Foo$": statics) of instance traits
avm::Traits *GameLayout::class_of(avm::Traits *instance)
{
    auto it = m_names.find(instance->name() + "$");
    if (it == m_names.end())
        return nullptr;
    for (avm::Traits *c : it->second)
    {
        if (c->pos_type == 1 && instance_of(c) == instance)
            return c;
    }
    return nullptr;
}

std::vector<avm::Traits *> GameLayout::by_name(const Spec &spec, bool statics)
{
    auto it = m_names.find(statics ? std::string(spec.cls) + "$" : std::string(spec.cls));
    return it == m_names.end() ? std::vector<avm::Traits *>() : it->second;
}

std::vector<avm::Traits *> GameLayout::by_key(uint64_t key, bool statics)
{
    std::vector<avm::Traits *> out;
    auto it = m_keys.find(key);
    if (it != m_keys.end())
    {
        for (avm::Traits *t : it->second)
        {
            if (t->pos_type == (statics ? 1 : 0))
                out.push_back(t);
        }
    }
    return out;
}

void GameLayout::resolve(avm::PoolObject *pool, avm::ScriptObject *main, const Spec *specs, size_t count,
                         bool by_structure)
{
    const auto started = std::chrono::steady_clock::now();
    clear();
    m_pool = pool;
    m_main = main;
    m_debug = getenv("TANOS_DEBUG") != nullptr;
    enumerate();
    for (avm::Traits *t : m_traits)
        m_keys[fingerprint(t)].push_back(t);

    std::vector<Found> found(count);
    for (size_t i = 0; i < count; i++)
        m_known[specs[i].kind].insert(specs[i].member ? specs[i].member : specs[i].cls);

    // 1) by name. A class name in several packages: the classes that have all its member specs
    if (!by_structure)
    {
        for (size_t i = 0; i < count; i++)
        {
            const Spec &spec = specs[i];
            if (spec.kind == CLASS)
            {
                std::vector<avm::Traits *> fit;
                for (avm::Traits *t : by_name(spec, false))
                {
                    bool all = true;
                    for (size_t j = 0; j < count && all; j++)
                    {
                        const Spec &m = specs[j];
                        if (m.kind == CLASS || std::string(m.cls) != spec.cls)
                            continue;
                        avm::Traits *start = m.statics ? class_of(t) : t;
                        Binding b;
                        uint16_t position;
                        all = start && find_member(start, m, b, position);
                    }
                    if (all)
                        fit.push_back(t);
                }
                if (fit.size() == 1)
                {
                    found[i].bindings.push_back(Binding { fit[0] });
                    found[i].key = fingerprint(fit[0]);
                    found[i].by_name = true;
                }
                continue;
            }
            // members: in every class of that name (the class spec, if any, already narrowed it)
            std::vector<avm::Traits *> starts;
            for (size_t j = 0; j < i; j++)
            {
                if (specs[j].kind == CLASS && std::string(specs[j].cls) == spec.cls && found[j].by_name)
                    starts.push_back(found[j].bindings[0].owner);
            }
            if (starts.empty())
                starts = by_name(spec, false);
            for (avm::Traits *t : starts)
            {
                avm::Traits *start = spec.statics ? class_of(t) : t;
                Binding b;
                uint16_t position = 0;
                if (!start || !find_member(start, spec, b, position))
                    continue;
                bool dup = false;
                for (auto &existing : found[i].bindings)
                    dup |= existing.owner == b.owner;
                if (dup)
                    continue;
                found[i].bindings.push_back(b);
                found[i].key = fingerprint(b.owner);
                found[i].position = position;
                found[i].by_name = true;
            }
        }
    }

    // 2) by structure (the table): for the specs no name found, and to check the table against
    // the names. A key must match as many classes as the table's classes with that key (two
    // classes may have the same structure, then both are meant).
    auto structure = [&](size_t i, std::vector<Binding> &out)
    {
        const Spec &spec = specs[i];
        const bool statics = spec.statics && spec.kind != CLASS;
        size_t expected = 0;
        for (size_t j = 0; j < count; j++)
            expected += specs[j].kind == CLASS && specs[j].key == spec.key;
        auto owners = spec.key ? by_key(spec.key, statics) : std::vector<avm::Traits *>();
        if (owners.empty() || owners.size() != std::max<size_t>(expected, 1))
            return false;
        for (avm::Traits *owner : owners)
        {
            Binding b;
            b.owner = owner;
            if (spec.kind != CLASS && !member_at(owner, spec, b))
                return false;
            out.push_back(b);
        }
        return true;
    };

    size_t by_key_count = 0, agree = 0;
    std::string missing, disagree;
    for (size_t i = 0; i < count; i++)
    {
        std::vector<Binding> bindings;
        const bool ok = structure(i, bindings);
        if (found[i].by_name)
        {
            // what the names found is among what the structure finds (classes of the same structure:
            // the structure finds all of them for each of their names)
            bool same = ok;
            for (auto &named : found[i].bindings)
            {
                bool match = false;
                for (auto &b : bindings)
                    match |= named.owner == b.owner && named.name == b.name;
                same = same && match;
            }
            agree += same;
            if (!same)
                disagree += (disagree.empty() ? "" : ", ") + spec_name(specs[i]);
            continue;
        }
        if (ok)
        {
            found[i].bindings = bindings;
            found[i].key = specs[i].key;
            found[i].position = specs[i].position;
            by_key_count++;
        }
        else
            missing += (missing.empty() ? "" : ", ") + spec_name(specs[i]);
    }

    for (size_t i = 0; i < count; i++)
    {
        const Spec &spec = specs[i];
        if (spec.kind == CLASS)
        {
            auto &classes = m_classes[spec.cls];
            for (auto &b : found[i].bindings)
                classes.push_back(b.owner);
        }
        else
        {
            auto &members = m_members[spec.kind][spec.member];
            members.insert(members.end(), found[i].bindings.begin(), found[i].bindings.end());
        }
    }

    size_t by_name_count = 0;
    for (size_t i = 0; i < count; i++)
        by_name_count += found[i].by_name;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (by_name_count)
        utils::log("[cleaner] layout: {} of {} found by name, the structure table finds {} of them ({} classes, {} ms)\n",
                   by_name_count, count, agree, m_traits.size(), ms);
    if (by_key_count || !by_name_count)
        utils::log("[cleaner] layout: {} of {} found by structure ({} classes, {} ms)\n",
                   by_key_count, count, m_traits.size(), ms);
    if (!missing.empty())
        utils::log("[cleaner] layout: not found: {}\n", missing);
    if (disagree.empty())
        return;
    utils::log("[cleaner] layout: the structure table doesn't match this client for {}{}\n", disagree,
               m_debug ? "" : " (TANOS_DEBUG=1 logs this client's table)");
    if (!m_debug)
        return;
    utils::log("[cleaner] layout: this client's structure table:\n");
    for (size_t i = 0; i < count; i++)
    {
        const Spec &spec = specs[i];
        const bool have = found[i].by_name;
        char key[24];
        snprintf(key, sizeof(key), "0x%016llx", static_cast<unsigned long long>(have ? found[i].key : spec.key));
        const std::string line = std::string("    { \"") + spec.cls + "\", "
            + (spec.member ? "\"" + std::string(spec.member) + "\"" : std::string("nullptr"))
            + ", GameLayout::" + KIND_NAMES[spec.kind] + ", " + (spec.statics ? "true" : "false") + ", " + key
            + ", " + std::to_string(have ? found[i].position : spec.position) + " },";
        utils::log("{}\n", line);
    }
}

const std::vector<avm::Traits *> &GameLayout::classes(const char *name) const
{
    static const std::vector<avm::Traits *> none;
    auto it = m_classes.find(name);
    return it == m_classes.end() ? none : it->second;
}

const GameLayout::Binding *GameLayout::member(avm::Traits *traits, const char *name, Kind kind) const
{
    auto it = m_members[kind].find(name);
    if (it == m_members[kind].end())
        return nullptr;
    for (int depth = 0; traits && depth < 32; traits = traits->base, depth++)
    {
        for (auto &b : it->second)
        {
            if (b.owner == traits)
                return &b;
        }
    }
    return nullptr;
}

bool GameLayout::known(const char *name, Kind kind) const
{
    return m_known[kind].count(name) != 0;
}

// The class' own QName in the pool (same name and namespace as its traits), then the definition:
// finddef/getproperty only ever get the exact name of a class that exists
avm::ScriptObject *GameLayout::class_object(avm::Traits *itraits)
{
    if (!itraits || !m_pool || !m_main)
        return nullptr;
    auto cached = m_class_objects.find(itraits);
    if (cached != m_class_objects.end())
        return cached->second;

    avm::ScriptObject *cls = nullptr;
    const std::string name = itraits->name();
    for (uintptr_t i = 0; i < m_pool->precomp_mn_size && !cls; i++)
    {
        avm::Multiname *mn = m_pool->get_multiname(i);
        if (!is_qname(mn) || mn->ns != itraits->ns || (mn->name != itraits->_name && mn->get_name() != name))
            continue;
        avm::ScriptObject *script = flash_stuff::finddef(m_main->vtable->einit, mn);
        if (!script)
            continue;
        Atom atom = flash_stuff::get_property(script, mn);
        if ((atom & 7) != 1 || !(atom & ~Atom(7)))
            continue;
        auto *candidate = reinterpret_cast<avm::ScriptObject *>(atom & ~Atom(7));
        if (candidate->vtable->ivtable && candidate->vtable->ivtable->traits == itraits)
            cls = candidate;
    }
    m_class_objects[itraits] = cls;
    return cls;
}
