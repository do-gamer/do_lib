#!/usr/bin/env python3
"""Covers the whole X screen with an override-redirect window, like a screen locker does,
until it gets SIGTERM/SIGINT. Stand-in for the lock screen in tests/display/run.sh.

Uses libX11 through ctypes: no X11 development headers or Python modules needed.
"""
import ctypes
import ctypes.util
import signal
import sys
import time

Display_p = ctypes.c_void_p
Window = ctypes.c_ulong


class XSetWindowAttributes(ctypes.Structure):
    _fields_ = [
        ('background_pixmap', ctypes.c_ulong), ('background_pixel', ctypes.c_ulong),
        ('border_pixmap', ctypes.c_ulong), ('border_pixel', ctypes.c_ulong),
        ('bit_gravity', ctypes.c_int), ('win_gravity', ctypes.c_int), ('backing_store', ctypes.c_int),
        ('backing_planes', ctypes.c_ulong), ('backing_pixel', ctypes.c_ulong), ('save_under', ctypes.c_int),
        ('event_mask', ctypes.c_long), ('do_not_propagate_mask', ctypes.c_long),
        ('override_redirect', ctypes.c_int), ('colormap', ctypes.c_ulong), ('cursor', ctypes.c_ulong),
    ]


CW_BACK_PIXEL, CW_OVERRIDE_REDIRECT = 1 << 1, 1 << 9
INPUT_OUTPUT = 1


def main():
    path = ctypes.util.find_library('X11')
    if not path:
        sys.exit('cover.py: libX11 not found')
    x = ctypes.CDLL(path)
    x.XOpenDisplay.restype = Display_p
    x.XOpenDisplay.argtypes = [ctypes.c_char_p]
    for name in ('XDefaultScreen', 'XDisplayWidth', 'XDisplayHeight'):
        getattr(x, name).argtypes = [Display_p] + ([ctypes.c_int] if name != 'XDefaultScreen' else [])
    x.XRootWindow.restype = Window
    x.XRootWindow.argtypes = [Display_p, ctypes.c_int]
    x.XCreateWindow.restype = Window
    x.XCreateWindow.argtypes = [Display_p, Window, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
                                ctypes.c_uint, ctypes.c_int, ctypes.c_uint, ctypes.c_void_p, ctypes.c_ulong,
                                ctypes.POINTER(XSetWindowAttributes)]
    for name in ('XMapRaised', 'XRaiseWindow', 'XDestroyWindow'):
        getattr(x, name).argtypes = [Display_p, Window]
    x.XFlush.argtypes = [Display_p]
    x.XCloseDisplay.argtypes = [Display_p]

    dpy = x.XOpenDisplay(None)
    if not dpy:
        sys.exit('cover.py: cannot open the X display')
    screen = x.XDefaultScreen(dpy)
    root = x.XRootWindow(dpy, screen)
    attrs = XSetWindowAttributes(background_pixel=0x101018, override_redirect=1)
    win = x.XCreateWindow(dpy, root, 0, 0, x.XDisplayWidth(dpy, screen), x.XDisplayHeight(dpy, screen), 0,
                          0, INPUT_OUTPUT, None, CW_BACK_PIXEL | CW_OVERRIDE_REDIRECT, ctypes.byref(attrs))
    x.XMapRaised(dpy, win)
    x.XFlush(dpy)

    running = True

    def stop(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    while running:
        time.sleep(0.5)
        x.XRaiseWindow(dpy, win)  # stay above windows mapped later (notifications, ...)
        x.XFlush(dpy)
    x.XDestroyWindow(dpy, win)
    x.XCloseDisplay(dpy)


if __name__ == '__main__':
    main()
