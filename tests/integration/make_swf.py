#!/usr/bin/env python3
"""Builds a minimal AS3 SWF for the integration test (no Flex SDK needed).

Document class (hand-assembled ABC):

    package {
        import flash.display.Sprite;
        public class Main extends Sprite {
            public function Main() {
                for (var i:int = 0; i < 2000000; i++) new Array(16);   // JIT + GC activity
            }
        }
    }

Loading it makes the flash plugin JIT-verify our methods (do_lib's verify_jit hook) and
free GC chunks (free_chunk hook) inside the real player.
"""
import struct
import sys


def u30(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def s24(v):
    return struct.pack('<i', v)[:3]


class Abc:
    def __init__(self):
        self.ints = []
        self.strings = []
        self.namespaces = []
        self.multinames = []

    def int(self, v):
        if v not in self.ints:
            self.ints.append(v)
        return self.ints.index(v) + 1

    def string(self, s):
        if s not in self.strings:
            self.strings.append(s)
        return self.strings.index(s) + 1

    def package_ns(self, name):
        entry = (0x16, self.string(name))  # PackageNamespace
        if entry not in self.namespaces:
            self.namespaces.append(entry)
        return self.namespaces.index(entry) + 1

    def qname(self, package, name):
        entry = (0x07, self.package_ns(package), self.string(name))
        if entry not in self.multinames:
            self.multinames.append(entry)
        return self.multinames.index(entry) + 1

    def pool(self):
        out = bytearray()
        out += u30(len(self.ints) + 1)
        for v in self.ints:
            out += u30(v)
        out += u30(0)  # uint
        out += u30(0)  # double
        out += u30(len(self.strings) + 1)
        for s in self.strings:
            b = s.encode()
            out += u30(len(b)) + b
        out += u30(len(self.namespaces) + 1)
        for kind, name in self.namespaces:
            out += bytes([kind]) + u30(name)
        out += u30(1)  # ns sets
        out += u30(len(self.multinames) + 1)
        for kind, ns, name in self.multinames:
            out += bytes([kind]) + u30(ns) + u30(name)
        return bytes(out)


# opcodes
GETLOCAL0, GETLOCAL1, SETLOCAL1 = 0xD0, 0xD1, 0xD5
PUSHSCOPE, POPSCOPE, RETURNVOID, POP = 0x30, 0x1D, 0x47, 0x29
CONSTRUCTSUPER, GETLEX, NEWCLASS, INITPROPERTY = 0x49, 0x60, 0x58, 0x68
GETSCOPEOBJECT, PUSHBYTE, PUSHINT, FINDPROPSTRICT = 0x65, 0x24, 0x2D, 0x5D
CONSTRUCTPROP, INCLOCAL_I, JUMP, IFLT, LABEL = 0x4A, 0xC2, 0x10, 0x15, 0x09


def build_abc():
    abc = Abc()
    main = abc.qname('', 'Main')
    chain = [
        abc.qname('', 'Object'),
        abc.qname('flash.events', 'EventDispatcher'),
        abc.qname('flash.display', 'DisplayObject'),
        abc.qname('flash.display', 'InteractiveObject'),
        abc.qname('flash.display', 'DisplayObjectContainer'),
        abc.qname('flash.display', 'Sprite'),
    ]
    sprite = chain[-1]
    array = abc.qname('', 'Array')
    limit = abc.int(2000000)

    # method 0: Main() constructor
    loop_body = bytes([FINDPROPSTRICT]) + u30(array) + bytes([PUSHBYTE, 16, CONSTRUCTPROP]) + u30(array) + u30(1) \
        + bytes([POP, INCLOCAL_I]) + u30(1)
    cond = bytes([GETLOCAL1, PUSHINT]) + u30(limit)
    ctor = bytearray([GETLOCAL0, PUSHSCOPE, GETLOCAL0, CONSTRUCTSUPER]) + u30(0)
    ctor += bytes([PUSHBYTE, 0, SETLOCAL1])
    ctor += bytes([JUMP]) + s24(1 + len(loop_body))      # jump to the condition (skip label + body)
    body_start = len(ctor)
    ctor += bytes([LABEL]) + loop_body
    ctor += cond
    iflt_pos = len(ctor) + 4                               # offset is relative to the next instruction
    ctor += bytes([IFLT]) + s24(body_start - iflt_pos)
    ctor += bytes([RETURNVOID])

    # method 1: class init, method 2: script init
    cinit = bytes([GETLOCAL0, PUSHSCOPE, RETURNVOID])
    sinit = bytearray([GETLOCAL0, PUSHSCOPE, GETSCOPEOBJECT, 0])
    for mn in chain:
        sinit += bytes([GETLEX]) + u30(mn) + bytes([PUSHSCOPE])
    sinit += bytes([GETLEX]) + u30(sprite) + bytes([NEWCLASS]) + u30(0)
    sinit += bytes([POPSCOPE] * len(chain))
    sinit += bytes([INITPROPERTY]) + u30(main) + bytes([RETURNVOID])

    out = bytearray(struct.pack('<HH', 16, 46))  # minor, major
    out += abc.pool()

    out += u30(3)  # methods: param_count, return_type, name, flags
    for _ in range(3):
        out += u30(0) + u30(0) + u30(0) + bytes([0])
    out += u30(0)  # metadata

    out += u30(1)  # classes
    # instance_info: name, super, flags (sealed), interfaces, iinit, traits
    out += u30(main) + u30(sprite) + bytes([0x01]) + u30(0) + u30(0) + u30(0)
    out += u30(1) + u30(0)  # class_info: cinit, traits

    out += u30(1)  # scripts: init, 1 class trait (slot 1 -> class 0)
    out += u30(2) + u30(1) + u30(main) + bytes([0x04]) + u30(1) + u30(0)

    bodies = [
        (0, 3, 2, 8, 9, bytes(ctor)),
        (1, 1, 1, 7, 8, cinit),
        (2, 3, 1, 1, 8, bytes(sinit)),
    ]
    out += u30(len(bodies))
    for method, max_stack, locals_, init_scope, max_scope, code in bodies:
        out += u30(method) + u30(max_stack) + u30(locals_) + u30(init_scope) + u30(max_scope)
        out += u30(len(code)) + code + u30(0) + u30(0)
    return bytes(out)


def tag(code, data):
    if len(data) < 0x3F:
        return struct.pack('<H', (code << 6) | len(data)) + data
    return struct.pack('<HI', (code << 6) | 0x3F, len(data)) + data


def build_swf():
    rect = bytes([0x78, 0x00, 0x05, 0x5F, 0x00, 0x00, 0x0F, 0xA0, 0x00])  # 0,0 - 550x400 twips
    body = rect + struct.pack('<HH', 24 << 8, 1)
    body += tag(69, struct.pack('<I', 0x08))                            # FileAttributes: AS3
    body += tag(9, bytes([0x20, 0x20, 0x30]))                           # SetBackgroundColor
    body += tag(82, struct.pack('<I', 1) + b'main\x00' + build_abc())   # DoABC (lazy init)
    body += tag(76, struct.pack('<HH', 1, 0) + b'Main\x00')             # SymbolClass: root = Main
    body += tag(1, b'')                                                 # ShowFrame
    body += tag(0, b'')                                                 # End
    return b'FWS' + bytes([11]) + struct.pack('<I', 8 + len(body)) + body


if __name__ == '__main__':
    data = build_swf()
    with open(sys.argv[1] if len(sys.argv) > 1 else 'test.swf', 'wb') as f:
        f.write(data)
    print(f'wrote {len(data)} bytes')
