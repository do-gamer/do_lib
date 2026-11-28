#ifndef FLASH_STUFF_H
#define FLASH_STUFF_H
#include <string>

#include "avm.h"

class flash_stuff
{
public:

    static bool install();
    static void uninstall();
    // Whether the hooks are installed in this process (i.e. this is the flash process)
    static bool installed();

    static void                 mouse_release(int x, int y, int button);
    static void                 mouse_press(int x, int y, int button);
    static bool                 hasproperty(avm::ScriptObject *obj, const std::string &prop_name);
    static uintptr_t            getproperty(Atom obj, avm::Multiname *mm, avm::VTable *vtable);
    // Toplevel::getproperty(obj, mn, obj->vtable): the AS3 read obj[mn] (getter or slot). Throws an
    // AS3 ReferenceError if a sealed object has no such property: only pass exact multinames.
    static Atom                 get_property(avm::ScriptObject *obj, avm::Multiname *mn);
    static void                 setproperty(avm::ScriptObject *obj, avm::Multiname *mm, Atom value);
    // Toplevel::setproperty(obj, mn, value, obj->vtable): the AS3 write obj[mn] = value (write
    // barriers and reference counts included). Same rule as get_property: exact multinames only.
    static void                 set_property(avm::ScriptObject *obj, avm::Multiname *mn, Atom value);
    static uintptr_t            gettraitsbinding(avm::Traits *traits);
    static uintptr_t            newarray(avm::MethodEnv *, uint32_t, void *);
    static avm::String          *newstring(avm::AvmCore *, const std::string &s);
    static avm::ScriptObject    *finddef(avm::MethodEnv *, avm::Multiname *);
    static avm::MethodSignature *get_method_signature(avm::MethodInfo *);

    // MMgc heap blocks in use, MB.
    static size_t               gc_heap_used_mb();
};

#endif // FLASH_STUFF_H
