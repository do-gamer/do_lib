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

With --report-input (mouse test) the constructor also does

                stage.scaleMode = "noScale"; stage.align = "TL";    // stage units = CSS px
                for each type in [mouseMove, mouseDown, mouseUp, click, mouseWheel]:
                    stage.addEventListener(type, onEvent);
            public function onEvent(e:*):void {
                ExternalInterface.call("report", "flash " + e.type + " " + e.stageX + "," + e.stageY + " " + e.delta);
            }

so the page sees every mouse event as the player gets it (a wheel event goes to whatever
is under the player's own mouse position, it carries no position from the browser).

With --stage3d (display test) it renders like the game does, through Stage3D (embed it
with wmode=direct), and reports every frame of its main thread to the page:

            public var ctx:*;
                stage.scaleMode = "noScale"; stage.align = "TL"; stage.frameRate = 60;
                var s3d:* = stage.stage3Ds[0];
                s3d.addEventListener("context3DCreate", onContext);  // again after a context loss
                s3d.requestContext3D();
                addEventListener("enterFrame", onFrame);
            public function onContext(e:*):void {
                ctx = e.target.context3D;
                ctx.configureBackBuffer(320, 240, 0, false);
                ExternalInterface.call("report", "stage3d " + ctx.driverInfo);
            }
            public function onFrame(e:*):void {
                ExternalInterface.call("tick", getTimer());   // first: a render error can't hide it
                if (ctx) { ctx.clear(); ctx.present(); }
            }
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
        self.ns_sets = []
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

    def public_index(self):
        """MultinameL in the public namespace: a runtime name, e.g. vector[i]."""
        ns_set = (self.package_ns(''),)
        if ns_set not in self.ns_sets:
            self.ns_sets.append(ns_set)
        entry = (0x1B, self.ns_sets.index(ns_set) + 1)
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
        out += u30(len(self.ns_sets) + 1)
        for ns_set in self.ns_sets:
            out += u30(len(ns_set)) + b''.join(u30(ns) for ns in ns_set)
        out += u30(len(self.multinames) + 1)
        for kind, *refs in self.multinames:
            out += bytes([kind]) + b''.join(u30(r) for r in refs)
        return bytes(out)


# opcodes
GETLOCAL0, GETLOCAL1, SETLOCAL1 = 0xD0, 0xD1, 0xD5
PUSHSCOPE, POPSCOPE, RETURNVOID, POP = 0x30, 0x1D, 0x47, 0x29
CONSTRUCTSUPER, GETLEX, NEWCLASS, INITPROPERTY = 0x49, 0x60, 0x58, 0x68
GETSCOPEOBJECT, PUSHBYTE, PUSHINT, FINDPROPSTRICT = 0x65, 0x24, 0x2D, 0x5D
CONSTRUCTPROP, INCLOCAL_I, JUMP, IFLT, LABEL = 0x4A, 0xC2, 0x10, 0x15, 0x09
PUSHSTRING, DUP, ADD, GETPROPERTY, SETPROPERTY, CALLPROPVOID = 0x2C, 0x2A, 0xA0, 0x66, 0x61, 0x4F
PUSHSHORT, PUSHFALSE, IFFALSE, CALLPROPERTY = 0x25, 0x27, 0x12, 0x46

INPUT_EVENTS = ['mouseMove', 'mouseDown', 'mouseUp', 'click', 'mouseWheel']


def build_abc(report_input=False, stage3d=False):
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
    pstr = lambda v: bytes([PUSHSTRING]) + u30(abc.string(v))
    prop = lambda op, name: bytes([op]) + u30(abc.qname('', name))
    call = lambda op, name, argc: bytes([op]) + u30(abc.qname('', name)) + u30(argc)
    external = lambda: abc.qname('flash.external', 'ExternalInterface')

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

    extra_methods = []  # (name multiname, body code, max_stack), methods 3, 4, ...
    instance_slots = []

    if report_input:
        on_event = abc.qname('', 'onEvent')  # first: keeps the constant pool (and the SWF) as before
        ctor += bytes([GETLOCAL0]) + prop(GETPROPERTY, 'stage')
        ctor += bytes([DUP]) + pstr('noScale') + prop(SETPROPERTY, 'scaleMode')
        ctor += bytes([DUP]) + pstr('TL') + prop(SETPROPERTY, 'align')
        for event in INPUT_EVENTS:
            ctor += bytes([DUP]) + pstr(event) + bytes([GETLOCAL0]) + prop(GETPROPERTY, 'onEvent')
            ctor += call(CALLPROPVOID, 'addEventListener', 2)
        ctor += bytes([POP])

        # onEvent(e)
        handler = bytearray([GETLOCAL0, PUSHSCOPE, GETLEX]) + u30(external())
        handler += pstr('report') + pstr('flash ')
        for name, sep in [('type', ' '), ('stageX', ','), ('stageY', ' '), ('delta', None)]:
            handler += bytes([GETLOCAL1]) + prop(GETPROPERTY, name) + bytes([ADD])
            if sep:
                handler += pstr(sep) + bytes([ADD])
        handler += call(CALLPROPVOID, 'call', 2) + bytes([RETURNVOID])
        extra_methods.append((on_event, bytes(handler), 6))

    if stage3d:
        instance_slots.append(abc.qname('', 'ctx'))
        ctor += bytes([GETLOCAL0]) + prop(GETPROPERTY, 'stage')
        ctor += bytes([DUP]) + pstr('noScale') + prop(SETPROPERTY, 'scaleMode')
        ctor += bytes([DUP]) + pstr('TL') + prop(SETPROPERTY, 'align')
        ctor += bytes([DUP, PUSHBYTE, 60]) + prop(SETPROPERTY, 'frameRate')
        ctor += prop(GETPROPERTY, 'stage3Ds') + bytes([PUSHBYTE, 0, GETPROPERTY]) + u30(abc.public_index())
        ctor += bytes([DUP]) + pstr('context3DCreate') + bytes([GETLOCAL0]) + prop(GETPROPERTY, 'onContext')
        ctor += call(CALLPROPVOID, 'addEventListener', 2)
        ctor += call(CALLPROPVOID, 'requestContext3D', 0)
        ctor += bytes([GETLOCAL0]) + pstr('enterFrame') + bytes([GETLOCAL0]) + prop(GETPROPERTY, 'onFrame')
        ctor += call(CALLPROPVOID, 'addEventListener', 2)

        # onContext(e)
        on_context = bytearray([GETLOCAL0, PUSHSCOPE])
        on_context += bytes([GETLOCAL0, GETLOCAL1]) + prop(GETPROPERTY, 'target') + prop(GETPROPERTY, 'context3D')
        on_context += prop(SETPROPERTY, 'ctx')
        on_context += bytes([GETLOCAL0]) + prop(GETPROPERTY, 'ctx')
        on_context += bytes([PUSHSHORT]) + u30(320) + bytes([PUSHSHORT]) + u30(240) + bytes([PUSHBYTE, 0, PUSHFALSE])
        on_context += call(CALLPROPVOID, 'configureBackBuffer', 4)
        on_context += bytes([GETLEX]) + u30(external()) + pstr('report') + pstr('stage3d ')
        on_context += bytes([GETLOCAL0]) + prop(GETPROPERTY, 'ctx') + prop(GETPROPERTY, 'driverInfo') + bytes([ADD])
        on_context += call(CALLPROPVOID, 'call', 2) + bytes([RETURNVOID])
        extra_methods.append((abc.qname('', 'onContext'), bytes(on_context), 6))

        # onFrame(e)
        get_timer = abc.qname('flash.utils', 'getTimer')
        on_frame = bytearray([GETLOCAL0, PUSHSCOPE])
        on_frame += bytes([GETLEX]) + u30(external()) + pstr('tick')
        on_frame += bytes([FINDPROPSTRICT]) + u30(get_timer) + bytes([CALLPROPERTY]) + u30(get_timer) + u30(0)
        on_frame += call(CALLPROPVOID, 'call', 2)
        draw = bytes([GETLOCAL0]) + prop(GETPROPERTY, 'ctx') + call(CALLPROPVOID, 'clear', 0)
        draw += bytes([GETLOCAL0]) + prop(GETPROPERTY, 'ctx') + call(CALLPROPVOID, 'present', 0)
        on_frame += bytes([GETLOCAL0]) + prop(GETPROPERTY, 'ctx') + bytes([IFFALSE]) + s24(len(draw)) + draw
        on_frame += bytes([RETURNVOID])
        extra_methods.append((abc.qname('', 'onFrame'), bytes(on_frame), 5))

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

    out += u30(3 + len(extra_methods))  # methods: param_count, return_type, [param types], name, flags
    for _ in range(3):
        out += u30(0) + u30(0) + u30(0) + bytes([0])
    for _ in extra_methods:
        out += u30(1) + u30(0) + u30(0) + u30(0) + bytes([0])  # handler(e:*):*
    out += u30(0)  # metadata

    out += u30(1)  # classes
    # instance_info: name, super, flags (sealed), interfaces, iinit, traits
    out += u30(main) + u30(sprite) + bytes([0x01]) + u30(0) + u30(0)
    out += u30(len(instance_slots) + len(extra_methods))
    for name in instance_slots:
        out += u30(name) + bytes([0x00]) + u30(0) + u30(0) + u30(0)  # slot: id (auto), type *, no value
    for i, (name, _, _) in enumerate(extra_methods):
        out += u30(name) + bytes([0x01]) + u30(0) + u30(3 + i)  # method trait: disp_id, method
    out += u30(1) + u30(0)  # class_info: cinit, traits

    out += u30(1)  # scripts: init, 1 class trait (slot 1 -> class 0)
    out += u30(2) + u30(1) + u30(main) + bytes([0x04]) + u30(1) + u30(0)

    ctor_stack = 6 if stage3d else 4 if report_input else 3
    bodies = [
        (0, ctor_stack, 2, 8, 9, bytes(ctor)),
        (1, 1, 1, 7, 8, cinit),
        (2, 3, 1, 1, 8, bytes(sinit)),
    ]
    for i, (_, code, max_stack) in enumerate(extra_methods):
        bodies.append((3 + i, max_stack, 2, 8, 9, code))
    out += u30(len(bodies))
    for method, max_stack, locals_, init_scope, max_scope, code in bodies:
        out += u30(method) + u30(max_stack) + u30(locals_) + u30(init_scope) + u30(max_scope)
        out += u30(len(code)) + code + u30(0) + u30(0)
    return bytes(out)


def tag(code, data):
    if len(data) < 0x3F:
        return struct.pack('<H', (code << 6) | len(data)) + data
    return struct.pack('<HI', (code << 6) | 0x3F, len(data)) + data


def build_swf(report_input=False, stage3d=False):
    rect = bytes([0x78, 0x00, 0x05, 0x5F, 0x00, 0x00, 0x0F, 0xA0, 0x00])  # 0,0 - 550x400 twips
    body = rect + struct.pack('<HH', (60 if stage3d else 24) << 8, 1)
    body += tag(69, struct.pack('<I', 0x08))                            # FileAttributes: AS3
    body += tag(9, bytes([0x20, 0x20, 0x30]))                           # SetBackgroundColor
    body += tag(82, struct.pack('<I', 1) + b'main\x00' + build_abc(report_input, stage3d))  # DoABC (lazy init)
    body += tag(76, struct.pack('<HH', 1, 0) + b'Main\x00')             # SymbolClass: root = Main
    body += tag(1, b'')                                                 # ShowFrame
    body += tag(0, b'')                                                 # End
    # Stage3D is versioned API: only visible to SWF 13 (Flash Player 11) and newer
    return b'FWS' + bytes([13 if stage3d else 11]) + struct.pack('<I', 8 + len(body)) + body


if __name__ == '__main__':
    flags = {'--report-input', '--stage3d'}
    args = [a for a in sys.argv[1:] if a not in flags]
    data = build_swf('--report-input' in sys.argv, '--stage3d' in sys.argv)
    with open(args[0] if args else 'test.swf', 'wb') as f:
        f.write(data)
    print(f'wrote {len(data)} bytes')
