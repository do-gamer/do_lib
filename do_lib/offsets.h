#ifndef OFFSETS_H
#define OFFSETS_H
#include <cstdint>

namespace offsets
{
    static const std::ptrdiff_t verifyjit            = 0x2fb430;
    static const std::ptrdiff_t free_chunk           = 0xb8a100;
    static const std::ptrdiff_t getproperty          = 0x326c30;
    static const std::ptrdiff_t setproperty          = 0x326a40;
    static const std::ptrdiff_t get_traits_binding   = 0x32b4c0;
    static const std::ptrdiff_t newarray             = 0x2def80;
    static const std::ptrdiff_t newstring            = 0x322f00;
    static const std::ptrdiff_t finddef              = 0x30fe50;
    static const std::ptrdiff_t input_thing_vt       = 0x131b710;
    static const std::ptrdiff_t mouse_release        = 0x72fe90;
    static const std::ptrdiff_t mouse_press          = 0x7306e0;
    static const std::ptrdiff_t get_method_sig       = 0x314430;

    // MMgc
    static const std::ptrdiff_t gcheap_instance_got  = 0x13d0800; // GOT slot holding &GCHeap::instance
    static const std::ptrdiff_t gcheap_num_alloc     = 0x5360;    // GCHeap::totalNumAlloc, blocks in use (4 KiB)

    // avmplus::MethodClosure : FunctionObject : ClassClosure (0x30 bytes, see avm::ClassClosure).
    // This build's FunctionObject has a code pointer at +0x30 before m_callEnv (measured in memory).
    static const std::ptrdiff_t closure_call_env     = 0x38;      // FunctionObject::m_callEnv (MethodEnv*)
    static const std::ptrdiff_t closure_saved_this   = 0x40;      // MethodClosure::m_savedThis (Atom)
}

#endif // OFFSETS_H
