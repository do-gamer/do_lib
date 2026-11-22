#!/usr/bin/env python3
"""Watches the game thread of a running bot through do_lib's flash IPC shared memory
(tools/flash_ipc.h), read-only, and writes one line per second:

    second at=<epoch ms> frames=<game thread ticks> max_gap_ms=<longest gap> installed=<0|1>

(the format of the display test page, see tests/integration/server.py). The heartbeat is the
CLOCK_MONOTONIC ms of the last tick of do_lib's GUI timer hook, i.e. of the game's own
timer, so gaps are measured on the game thread itself. Follows the flash process across
refreshes. Runs until the stop file exists.

    heartbeat.py <out file> <stop file>
"""
import ctypes
import os
import sys
import time

MAGIC = 0x54414E32  # "TAN2"
OFF_MAGIC, OFF_HEARTBEAT, OFF_INSTALLED = 0, 16, 24  # flash_ipc::Shared layout
SHM_RDONLY = 0o10000

libc = ctypes.CDLL(None, use_errno=True)
libc.shmget.restype = ctypes.c_int
libc.shmget.argtypes = [ctypes.c_int, ctypes.c_size_t, ctypes.c_int]
libc.shmat.restype = ctypes.c_void_p
libc.shmat.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
libc.shmdt.argtypes = [ctypes.c_void_p]


def flash_pids():
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            with open(f'/proc/{pid}/cmdline', 'rb') as f:
                cmd = f.read()
        except OSError:
            continue
        if b'--type=ppapi' in cmd and b'ppapi-broker' not in cmd:
            yield int(pid)


def attach():
    """Maps the segment do_lib created (key = flash pid); returns (pid, address) or None."""
    for pid in flash_pids():
        shmid = libc.shmget(pid, 0, 0)
        if shmid < 0:
            continue
        addr = libc.shmat(shmid, None, SHM_RDONLY)
        if addr in (None, ctypes.c_void_p(-1).value):
            continue
        if ctypes.c_uint32.from_address(addr + OFF_MAGIC).value == MAGIC:
            return pid, addr
        libc.shmdt(addr)
    return None


def main():
    out_path, stop_path = sys.argv[1], sys.argv[2]
    out = open(out_path, 'a', buffering=1)
    seg = None
    last_hb = frames = max_gap = 0
    next_report = time.monotonic() + 1
    while not os.path.exists(stop_path):
        if seg is None or not os.path.exists(f'/proc/{seg[0]}') \
                or ctypes.c_uint32.from_address(seg[1] + OFF_MAGIC).value != MAGIC:
            if seg is not None:
                libc.shmdt(seg[1])
                out.write(f'detached pid={seg[0]} at={int(time.time() * 1000)}\n')
            seg = attach()
            last_hb = 0
            if seg is not None:
                out.write(f'attached pid={seg[0]} at={int(time.time() * 1000)}\n')
            else:
                time.sleep(0.5)

        if seg is not None:
            hb = ctypes.c_uint64.from_address(seg[1] + OFF_HEARTBEAT).value
            if hb != last_hb:
                if last_hb:
                    frames += 1
                    max_gap = max(max_gap, hb - last_hb)
                last_hb = hb

        now = time.monotonic()
        if now >= next_report:
            installed = ctypes.c_uint32.from_address(seg[1] + OFF_INSTALLED).value if seg else 0
            # a stall still going on counts up to now (heartbeat is CLOCK_MONOTONIC ms, like monotonic())
            open_gap = int(now * 1000) - last_hb if last_hb else 0
            out.write(f'second at={int(time.time() * 1000)} frames={frames} '
                      f'max_gap_ms={max(max_gap, open_gap)} installed={installed}\n')
            frames = max_gap = 0
            next_report = max(next_report + 1, now + 0.5)  # never a burst of reports after a pause
        time.sleep(0.002)


if __name__ == '__main__':
    main()
