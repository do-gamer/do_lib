#include "avm.h"
#include "binary_stream.h"
#include "utils.h"
#include <mutex>
#include <unordered_map>

avm::ClassClosure * avm::AbcEnv::finddef(const std::string &name)
{
    return finddef([name] (avm::ClassClosure *closure)
    {
        return closure->get_name() == name;
    });
}

avm::ClassClosure * avm::AbcEnv::finddef(std::function<bool(avm::ClassClosure *)> pred)
{
    std::vector<avm::ClassClosure *> results;
    for (size_t i = 0; i < finddef_table->capacity; i++)
    {
        avm::ScriptObject *obj = finddef_table->data[i];

        if (!obj)
        {
            continue;
        }

        auto *closure = obj->get_at<avm::ClassClosure *>(0x20);

        if (reinterpret_cast<uintptr_t>(closure) <= 0x200000001 || (reinterpret_cast<uintptr_t>(closure) & 7) != 0)
        {
            continue;
        }

        if (pred(closure))
        {
            return closure;
        }
    }
    return nullptr;
}

std::string avm::MethodInfo::name()
{
    static std::mutex cache_mutex;
    static std::unordered_map<const avm::MethodInfo *, std::string> cache;

    // quick-path: check cache under lock, but do expensive work outside
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(this);
        if (it != cache.end())
            return it->second;
    }

    // compute resolved name without holding the mutex
    std::string resolved_name = pool->get_method_name(id);

    if (!resolved_name.empty() && declarer.is_traits())
    {
        auto *traits = declarer.traits();
        if (traits && traits->traits_pos)
        {
            BinaryStream s { traits->traits_pos };
            uint32_t trait_count = 0;

            switch (traits->pos_type)
            {
                case 0: // instance_info
                {
                    /* name */ s.read_u32();
                    /* super_name */ s.read_u32();

                    auto flags = s.read_u32();
                    if ((flags & 0x8) != 0)
                    {
                        /* protected_ns */ s.read_u32();
                    }

                    auto interface_count = s.read_u32();
                    for (uint32_t i = 0; i < interface_count; i++)
                    {
                        /* interface */ s.read_u32();
                    }

                    /* iinit */ s.read_u32();
                    trait_count = s.read_u32();
                    break;
                }
                case 1: // class_info
                case 2: // script_info
                {
                    /* cinit/init */ s.read_u32();
                    trait_count = s.read_u32();
                    break;
                }
                default:
                {
                    break;
                }
            }

            for (uint32_t j = 0; j < trait_count; j++)
            {
                /* name */ s.read_u32();
                unsigned char tag = s.read<uint8_t>();
                int kind = (tag & avm::TRAIT_mask);

                switch(kind)
                {
                    case avm::TRAIT_Slot:
                    case avm::TRAIT_Const:
                    {
                        /* slot_id */ s.read_u32();
                        /* type_name */ s.read_u32();
                        uint32_t vindex = s.read_u32();
                        if (vindex)
                        {
                            /* vkind */ s.read<uint8_t>();
                        }
                        break;
                    }
                    case avm::TRAIT_Class:
                    case avm::TRAIT_Function:
                    {
                        /* slot_id */ s.read_u32();
                        /* class/function index */ s.read_u32();
                        break;
                    }
                    case avm::TRAIT_Method:
                    case avm::TRAIT_Getter:
                    case avm::TRAIT_Setter:
                    {
                        /* disp_id */ s.read_u32();
                        uint32_t method_index = s.read_u32();

                        if (static_cast<int32_t>(method_index) == id)
                        {
                            switch (kind)
                            {
                                case avm::TRAIT_Setter:
                                    resolved_name = "set " + resolved_name;
                                    break;
                                case avm::TRAIT_Getter:
                                    resolved_name = "get " + resolved_name;
                                    break;
                                default:
                                    break;
                            }

                            // insert into cache and return
                            std::lock_guard<std::mutex> lock(cache_mutex);
                            cache.try_emplace(this, resolved_name);
                            return resolved_name;
                        }
                        break;
                    }
                    default:
                    {
                        break;
                    }
                }

                if (tag & avm::ATTR_metadata)
                {
                    uint32_t metadata_count = s.read_u32();
                    for (uint32_t i = 0; i < metadata_count; i++)
                    {
                        /* metadata index */ s.read_u32();
                    }
                }
            }
        }
    }

    // insert final resolved name into cache and return
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto [it, inserted] = cache.try_emplace(this, resolved_name);
        if (!inserted)
            return it->second;
        return it->second;
    }
}

#include <mutex>
#include <unordered_map>

static std::mutex g_traits_cache_mutex;
static std::unordered_map<const avm::Traits *, avm::MyTraits> g_traits_cache;

avm::MyTraits avm::Traits::parse_traits(avm::PoolObject *custom_pool)
{
    // fast-path: check global cache
    {
        std::lock_guard<std::mutex> lock(g_traits_cache_mutex);
        auto it = g_traits_cache.find(this);
        if (it != g_traits_cache.end())
        {
            return it->second; // copy
        }
    }

    BinaryStream s { traits_pos };
    custom_pool = custom_pool ? custom_pool : pool;
    MyTraits traits;

    /* auto qname = */ s.read_u32();
    /* auto qname = */ s.read_u32();

    auto flags = s.read_u32();

    if ((flags & 0x8) != 0)
    {
        s.read_u32();
    }

    auto interface_count = s.read_u32();
    for (uint32_t i = 0; i < interface_count; i++)
    {
        s.read_u32();
    }

    /* auto iinit = */ s.read_u32();

    uint32_t trait_count = s.read_u32();
    traits.traits.reserve(trait_count); // avoid repeated reallocations

    for (uint32_t j = 0; j < trait_count; j++)
    {
        MyTrait trait;

        uint32_t name = s.read_u32();
        unsigned char tag = s.read<uint8_t>();
        auto kind = avm::TraitKind(tag & 0xf);

        avm::Multiname *mn = custom_pool->get_multiname(name);
        trait.name_index = name;
        trait.name = (mn) ? mn->get_name() : "";
        trait.kind = kind;

        switch(kind)
        {
            case avm::TRAIT_Slot:
            case avm::TRAIT_Const:
            {
                /* uint32_t slot_id    = */ s.read_u32();
                uint32_t type_name  = s.read_u32();
                uint32_t vindex     = s.read_u32(); // references one of the tables in the constant pool, depending on the value of vkind
                trait.id = vindex;
                trait.type_id = type_name;

                if (vindex)
                {
                    /*uint8_t vkind = */ s.read<uint8_t>(); // ignored by the avm
                }

                break;
            }
            case avm::TRAIT_Class:
            {
                /* uint32_t slot_id = */ s.read_u32();
                uint32_t class_index = s.read_u32(); //  is an index that points into the class array of the abcFile entry
                trait.id = class_index;
                break;
            }
            case avm::TRAIT_Function:
            {
                /* uint32_t slot_id = */ s.read_u32();
                uint32_t function_index = s.read_u32();
                trait.id = function_index;
                break;
            }
            case avm::TRAIT_Method:
            case avm::TRAIT_Getter:
            case avm::TRAIT_Setter:
            {
                // The disp_id field is a compiler assigned integer that is used by the AVM2 to optimize the resolution of
                // virtual function calls. An overridden method must have the same disp_id as that of the method in the 
                // base class. A value of zero disables this optimization.
                /*uint32_t disp_id = */s.read_u32();
                uint32_t method_index = s.read_u32(); // is an index that points into the method array of the abcFile e
                trait.id = method_index;
                trait.temp = name;
                break;
            }
            default:
            {
                utils::log("Invalid trait\n");
                break;
            }
        }

        if (tag & avm::ATTR_metadata)
        {
            uint32_t metadata_count  = s.read_u32();
            for (uint32_t i = 0; i < metadata_count; i++)
            {
                /*uint32_t index = */ s.read_u32();
            }
        }

        traits.add_trait(trait);
    }

    // insert into cache
    {
        std::lock_guard<std::mutex> lock(g_traits_cache_mutex);
        g_traits_cache.try_emplace(this, traits);
    }

    return traits;
}

void avm::clear_traits_cache()
{
    std::lock_guard<std::mutex> lock(g_traits_cache_mutex);
    g_traits_cache.clear();
}

