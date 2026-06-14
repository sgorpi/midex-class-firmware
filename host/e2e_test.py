#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2026 Hedde Bosman (sgorpi@gmail.com)
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

"""End-to-end test harness for the class-compliant MIDEX8 firmware.

Drives the device's ALSA rawmidi ports directly (hw:CARD,0,SUBDEV) via libasound
through ctypes -- no pip deps, low/consistent latency, the same raw path `amidi`
uses. With a physical MIDI cable looping a port's DIN OUT back into its DIN IN,
anything sent on that ALSA port comes straight back, so we can check correctness
and measure host-to-host round-trip timing.

Subcommands (run with a loopback cable on the port under test):
  functional   channel-voice + system-common round-trip, incl. running status
  sysex        short / 3-multiple / long-multi-USB-packet / interleaved-realtime
  timing       round-trip latency distribution (min/mean/median/p95/p99/max/sd)
  jitter       send at a fixed interval, measure received inter-arrival spread
  throughput   sustained send rate vs loss (UART ceiling ~1040 3-byte msg/s/port)
  soak         long run, count drops/corruptions
  isolation    send on one port, confirm the OTHER ports stay silent (no crosstalk)
  selftest     validate the MIDI canonicalizer (no hardware needed)
  all          functional + sysex + a quick timing pass

Examples:
  ./e2e_test.py selftest
  ./e2e_test.py -p 2 functional
  ./e2e_test.py -p 2 timing --count 2000
  ./e2e_test.py -p 2 jitter --interval-ms 10 --count 1000
  ./e2e_test.py -p 2 throughput --duration 5
  ./e2e_test.py isolation --ports 1,2,3

The same harness also drives the STOCK firmware (custom snd-usb-midex driver,
which names its ports "MIDEX Port N"): add `-m "MIDEX Port"`. Handy for comparing
stock vs class-compliant latency/jitter. On stock, the firmware RX-overflow
counter is unavailable (it is a class-fw vendor request) and real-time bytes are
passed through unfiltered, but the canonicalizer drops them so the checks hold.

Notes / caveats:
  * System real-time bytes (>=0xF8: clock F8, sensing FE, ...) are FILTERED by
    snd-usbmidi on the INPUT side, so they never come back through ALSA even
    though the device echoes them. The harness expects this and does not fail on
    it (verify real-time pass-through with usbmon, not here).
  * The device expands running status (USB-MIDI packets are self-contained), so
    the harness compares *canonicalized messages*, not raw bytes.
  * Round-trip latency is the full host->USB->fw->UART-TX->DIN->cable->DIN->
    UART-RX->fw->USB->host path (~half is "out", ~half is "in").
"""
import argparse
import ctypes
import ctypes.util
import select
import statistics
import subprocess
import sys
import time

# --------------------------------------------------------------------------
# libasound rawmidi binding (ctypes)
# --------------------------------------------------------------------------
_lib = ctypes.util.find_library("asound") or "libasound.so.2"
asound = ctypes.CDLL(_lib)

SND_RAWMIDI_NONBLOCK = 1

asound.snd_rawmidi_open.argtypes = [
    ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p),
    ctypes.c_char_p, ctypes.c_int]
asound.snd_rawmidi_open.restype = ctypes.c_int
asound.snd_rawmidi_close.argtypes = [ctypes.c_void_p]
asound.snd_rawmidi_nonblock.argtypes = [ctypes.c_void_p, ctypes.c_int]
asound.snd_rawmidi_nonblock.restype = ctypes.c_int
asound.snd_rawmidi_write.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
asound.snd_rawmidi_write.restype = ctypes.c_long
asound.snd_rawmidi_read.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
asound.snd_rawmidi_read.restype = ctypes.c_long
asound.snd_rawmidi_drain.argtypes = [ctypes.c_void_p]
asound.snd_rawmidi_poll_descriptors_count.argtypes = [ctypes.c_void_p]
asound.snd_rawmidi_poll_descriptors_count.restype = ctypes.c_int
asound.snd_strerror.argtypes = [ctypes.c_int]
asound.snd_strerror.restype = ctypes.c_char_p


class _pollfd(ctypes.Structure):
    _fields_ = [("fd", ctypes.c_int),
                ("events", ctypes.c_short),
                ("revents", ctypes.c_short)]


asound.snd_rawmidi_poll_descriptors.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(_pollfd), ctypes.c_uint]
asound.snd_rawmidi_poll_descriptors.restype = ctypes.c_int


def _chk(ret, what):
    if ret < 0:
        raise OSError(f"{what}: {asound.snd_strerror(ret).decode()}")
    return ret


class RawPort:
    """One MIDEX ALSA rawmidi port. Opens input + output in a single
    snd_rawmidi_open: this device's hw rawmidi rejects NONBLOCK at open (asserts
    on input-only, EINVAL otherwise), so we open BLOCKING and then switch the
    input handle to non-blocking at runtime (so reads return whatever is pending
    instead of blocking for a full buffer). A poller lets us wait with a timeout
    and stamp arrival; the output handle stays blocking."""

    def __init__(self, hwname):
        self.hwname = hwname
        self._in = ctypes.c_void_p()
        self._out = ctypes.c_void_p()
        _chk(asound.snd_rawmidi_open(ctypes.byref(self._in),
                                     ctypes.byref(self._out),
                                     hwname.encode(), 0),
             f"open {hwname}")
        _chk(asound.snd_rawmidi_nonblock(self._in, 1), "nonblock in")
        n = _chk(asound.snd_rawmidi_poll_descriptors_count(self._in), "pollcount")
        pfds = (_pollfd * n)()
        _chk(asound.snd_rawmidi_poll_descriptors(self._in, pfds, n), "polldesc")
        self._poll = select.poll()
        for i in range(n):
            self._poll.register(pfds[i].fd, select.POLLIN)
        self._buf = (ctypes.c_ubyte * 1024)()

    def write(self, data):
        """Write all bytes (non-blocking handle: loop over short writes)."""
        data = bytes(data)
        off = 0
        while off < len(data):
            n = asound.snd_rawmidi_write(self._out, data[off:], len(data) - off)
            if n == -11:                       # -EAGAIN: out buffer full
                time.sleep(0.0002)
                continue
            _chk(n, "write")
            off += n
        return off

    def read_ready(self):
        """Non-blocking read of whatever is pending; returns bytes (maybe b'')."""
        n = asound.snd_rawmidi_read(self._in, self._buf, 1024)
        if n == -11:                       # -EAGAIN: nothing pending
            return b""
        if n < 0:
            raise OSError(f"read: {asound.snd_strerror(n).decode()}")
        return bytes(self._buf[:n])

    def drain_input(self, settle=0.05):
        """Discard any buffered input (e.g. stale echoes) before a test."""
        end = time.perf_counter() + settle
        while time.perf_counter() < end:
            if self._poll.poll(0):
                self.read_ready()

    def collect(self, want_bytes, timeout):
        """Read until `want_bytes` bytes received or timeout. Returns
        (data, t_first_byte, t_last_byte) with perf_counter timestamps."""
        data = bytearray()
        t_first = t_last = None
        deadline = time.perf_counter() + timeout
        while len(data) < want_bytes:
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                break
            if self._poll.poll(remaining * 1000.0):
                chunk = self.read_ready()
                if chunk:
                    now = time.perf_counter()
                    if t_first is None:
                        t_first = now
                    t_last = now
                    data += chunk
        return bytes(data), t_first, t_last

    def collect_idle(self, idle=0.03, maxwait=1.0):
        """Read until the input goes quiet for `idle` s (after receiving at least
        one byte) or `maxwait` elapses. Robust to running-status compression,
        where the echoed byte count differs from what was sent. Returns
        (data, t_first, t_last)."""
        data = bytearray()
        t_first = t_last = None
        start = time.perf_counter()
        while time.perf_counter() - start < maxwait:
            if self._poll.poll(idle * 1000.0):
                chunk = self.read_ready()
                if chunk:
                    now = time.perf_counter()
                    if t_first is None:
                        t_first = now
                    t_last = now
                    data += chunk
            elif data:
                break                          # idle gap after some data
        return bytes(data), t_first, t_last

    def collect_window(self, window):
        """Read everything arriving for `window` seconds, timestamping chunks.
        Returns list of (timestamp, bytes)."""
        out = []
        deadline = time.perf_counter() + window
        while True:
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                break
            if self._poll.poll(remaining * 1000.0):
                chunk = self.read_ready()
                if chunk:
                    out.append((time.perf_counter(), chunk))
        return out

    def close(self):
        if self._in:
            asound.snd_rawmidi_close(self._in)
            self._in = ctypes.c_void_p()
        if self._out:
            asound.snd_rawmidi_close(self._out)
            self._out = ctypes.c_void_p()


# --------------------------------------------------------------------------
# MIDI canonicalizer  (pure, unit-tested by `selftest`)
# --------------------------------------------------------------------------
def _vlen(status):
    """Data-byte count for a status byte (channel voice + system common)."""
    h = status & 0xF0
    if h in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
        return 2
    if h in (0xC0, 0xD0):
        return 1
    if status in (0xF1, 0xF3):
        return 1
    if status == 0xF2:
        return 2
    return 0                                   # F6 tune request, etc.


def canonicalize(data, drop_realtime=True):
    """Split a MIDI byte stream into a list of complete messages (as bytes),
    expanding running status so each channel message carries its status byte.
    SysEx is one F0..F7 message. Real-time bytes (>=0xF8) are dropped by default
    (snd-usbmidi filters them on input)."""
    msgs = []
    status = 0
    cur = bytearray()
    sysex = bytearray()
    in_sysex = False
    for b in data:
        if b >= 0xF8:                          # real-time: anywhere
            if not drop_realtime:
                msgs.append(bytes([b]))
            continue
        if in_sysex:
            sysex.append(b)
            if b == 0xF7:
                msgs.append(bytes(sysex))
                sysex = bytearray()
                in_sysex = False
            continue
        if b == 0xF0:                          # SysEx start
            status = 0
            in_sysex = True
            sysex = bytearray([0xF0])
            continue
        if b >= 0x80:                          # status byte (not RT, not F0)
            if b == 0xF7:                      # stray end: ignore
                status = 0
                continue
            need = _vlen(b)
            if need == 0:                      # single-byte system common (F6)
                msgs.append(bytes([b]))
                status = 0
            else:
                status = b
                cur = bytearray([b])
            continue
        # data byte
        if status == 0:
            continue                           # orphan data: drop
        cur.append(b)
        if len(cur) == 1 + _vlen(status):
            msgs.append(bytes(cur))
            if status >= 0xF0:                 # system common: no running status
                status = 0
                cur = bytearray()
            else:
                cur = bytearray([status])      # channel voice: running status
    return msgs


def hx(b):
    return " ".join(f"{x:02X}" for x in b)


def hxlist(msgs):
    return "  ".join("[" + hx(m) + "]" for m in msgs)


# --------------------------------------------------------------------------
# port discovery
# --------------------------------------------------------------------------
def find_ports(match="MIDEX8 UAC"):
    """Return {port_number(1-based): "hw:C,0,S"} from `amidi -l`."""
    out = subprocess.run(["amidi", "-l"], capture_output=True, text=True).stdout
    ports = {}
    import re
    for line in out.splitlines():
        m = re.match(r"\s*(?:IO|I|O)\s+(hw:\S+)\s+(.*\S)\s*$", line)
        if not m:
            continue
        addr, name = m.group(1), m.group(2)
        if match.lower() not in name.lower():
            continue
        # class fw names ports "MIDEX8 UAC MIDI 1"; the stock snd-usb-midex
        # driver names them "MIDEX Port 1" -- accept either trailing keyword.
        mn = re.search(r"(?:MIDI|Port)\s+(\d+)\s*$", name)
        if mn and "," in addr:
            ports[int(mn.group(1))] = addr
    return ports


def open_port(args, port=None):
    port = port or args.port
    ports = find_ports(args.match)
    if port not in ports:
        sys.exit(f"port {port} not found. amidi -l shows: {sorted(ports)}")
    return RawPort(ports[port])


# --------------------------------------------------------------------------
# firmware RX-overflow counter (vendor control request; needs pyusb)
# --------------------------------------------------------------------------
VENDOR_VID, VENDOR_PID = 0x0A4E, 0x10C1
VENDOR_REQ_GET_RX_OVERFLOWS = 0x01
VENDOR_REQ_CLR_RX_OVERFLOWS = 0x02


def _vendor_dev():
    """The MIDEX8 USB device for vendor control transfers, or None if pyusb or
    the device is unavailable (the MIDI checks still run without it)."""
    try:
        import usb.core
    except ImportError:
        return None
    return usb.core.find(idVendor=VENDOR_VID, idProduct=VENDOR_PID)


def read_rx_overflows():
    """Return the firmware's saturating RX-overflow byte, or None if it can't be
    read (no pyusb / no device / transfer error)."""
    dev = _vendor_dev()
    if dev is None:
        return None
    try:
        ret = dev.ctrl_transfer(0xC0, VENDOR_REQ_GET_RX_OVERFLOWS, 0, 0, 1)
        return int(ret[0])
    except Exception as e:                                  # noqa: BLE001
        print(f"  (rx-overflow read failed: {e})")
        return None


def clear_rx_overflows():
    dev = _vendor_dev()
    if dev is None:
        return
    try:
        dev.ctrl_transfer(0x40, VENDOR_REQ_CLR_RX_OVERFLOWS, 0, 0, None)
    except Exception as e:                                  # noqa: BLE001
        print(f"  (rx-overflow clear failed: {e})")


# --------------------------------------------------------------------------
# round-trip helper
# --------------------------------------------------------------------------
def roundtrip(rp, send_bytes, drop_realtime=True, idle=0.03, maxwait=1.0):
    """Send send_bytes, collect the echo until the line goes quiet, and compare
    canonicalized messages (robust to running-status compression). Returns
    (ok, expected, got, latency_s, raw_received_bytes)."""
    expected = canonicalize(send_bytes, drop_realtime=drop_realtime)
    rp.drain_input(0.02)
    t0 = time.perf_counter()
    rp.write(send_bytes)
    data, t_first, t_last = rp.collect_idle(idle, maxwait)
    got = canonicalize(data, drop_realtime=drop_realtime)
    lat = (t_last - t0) if t_last else None
    return got == expected, expected, got, lat, data


# --------------------------------------------------------------------------
# subcommands
# --------------------------------------------------------------------------
def cmd_functional(args):
    rp = open_port(args)
    cases = [
        ("note on",            bytes([0x90, 0x3C, 0x7F])),
        ("note off (8n)",      bytes([0x80, 0x3C, 0x40])),
        ("note off (9n v0)",   bytes([0x90, 0x3C, 0x00])),
        ("poly aftertouch",    bytes([0xA0, 0x3C, 0x55])),
        ("control change",     bytes([0xB0, 0x07, 0x64])),
        ("program change",     bytes([0xC0, 0x05])),
        ("channel aftertouch", bytes([0xD0, 0x40])),
        ("pitch bend",         bytes([0xE0, 0x00, 0x40])),
        ("running status x3",  bytes([0x90, 0x3C, 0x7F, 0x3E, 0x7F, 0x40, 0x7F])),
        ("running status CC",  bytes([0xB0, 0x07, 0x64, 0x0A, 0x40])),
        ("MTC quarter frame",  bytes([0xF1, 0x23])),
        ("song position",      bytes([0xF2, 0x12, 0x34])),
        ("song select",        bytes([0xF3, 0x05])),
        ("tune request",       bytes([0xF6])),
    ]
    print(f"functional round-trip on {rp.hwname} (loop DIN OUT->IN):\n")
    npass = 0
    for name, msg in cases:
        ok, exp, got, lat, raw = roundtrip(rp, msg)
        npass += ok
        lt = f"{lat*1e3:6.2f} ms" if lat else "   --  "
        print(f"  {name:20s} sent [{hx(msg)}] -> {hxlist(got) or '(nothing)':24s}"
              f" {lt}  {'OK' if ok else 'FAIL'}")
        if not ok:
            print(f"      expected {hxlist(exp)}   raw recv [{hx(raw)}]")
    print(f"\n=> {npass}/{len(cases)} passed.")
    rp.close()
    return 0 if npass == len(cases) else 1


def cmd_sysex(args):
    rp = open_port(args)
    long_payload = bytes((i & 0x7F) for i in range(200))   # > 3 USB packets
    cases = [
        ("short (1 data)",     bytes([0xF0, 0x7D, 0x01, 0xF7])),
        ("empty",              bytes([0xF0, 0xF7])),
        ("3-byte multiple",    bytes([0xF0, 0x7D, 0x11, 0x22, 0x33, 0x44, 0xF7])),
        ("long 200B multi-pkt", bytes([0xF0, 0x7D]) + long_payload + bytes([0xF7])),
        ("realtime interleaved",
         bytes([0xF0, 0x7D, 0x11, 0xF8, 0x22, 0x33, 0xF7])),
    ]
    print(f"sysex round-trip on {rp.hwname}:\n")
    npass = 0
    for name, msg in cases:
        ok, exp, got, lat, raw = roundtrip(rp, msg, maxwait=1.5)
        npass += ok
        lt = f"{lat*1e3:6.2f} ms" if lat else "   --  "
        shown = hxlist(got)
        if len(shown) > 60:
            shown = shown[:57] + "..."
        print(f"  {name:22s} {lt}  {'OK' if ok else 'FAIL'}  got {shown}")
        if not ok:
            print(f"      expected {hxlist(exp)[:100]}")
            print(f"      raw recv [{hx(raw)[:100]}]")
    print(f"\n=> {npass}/{len(cases)} passed.  (realtime interleaved: the F8 is "
          "filtered by the host input, so it is expected to be absent.)")
    rp.close()
    return 0 if npass == len(cases) else 1


def _latency_samples(rp, count, msg, gap=0.005, timeout=0.5):
    lats, drops, corrupt = [], 0, 0
    expected = canonicalize(msg)
    want = sum(len(m) for m in expected)
    for _ in range(count):
        rp.drain_input(0.0)
        t0 = time.perf_counter()
        rp.write(msg)
        data, t_first, t_last = rp.collect(want, timeout)
        if t_last is None:
            drops += 1
        elif canonicalize(data) != expected:
            corrupt += 1
        else:
            lats.append((t_last - t0) * 1e3)    # ms
        if gap:
            time.sleep(gap)
    return lats, drops, corrupt


def _stats(name, xs):
    if not xs:
        print(f"  {name}: no samples")
        return
    xs_sorted = sorted(xs)
    def pct(p):
        return xs_sorted[min(len(xs_sorted) - 1, int(p / 100 * len(xs_sorted)))]
    print(f"  {name} (n={len(xs)}):")
    print(f"    min {min(xs):6.2f}  mean {statistics.mean(xs):6.2f}  "
          f"median {statistics.median(xs):6.2f}  p95 {pct(95):6.2f}  "
          f"p99 {pct(99):6.2f}  max {max(xs):6.2f}  "
          f"sd {statistics.pstdev(xs):5.2f}  (ms)")


def cmd_timing(args):
    rp = open_port(args)
    msg = bytes([0x90, 0x3C, 0x7F])
    print(f"timing on {rp.hwname}: {args.count} round-trips of [{hx(msg)}]\n")
    lats, drops, corrupt = _latency_samples(rp, args.count, msg)
    _stats("round-trip latency", lats)
    print(f"  drops {drops}  corrupt {corrupt}")
    rp.close()
    return 0 if drops == 0 and corrupt == 0 else 1


def cmd_jitter(args):
    rp = open_port(args)
    msg = bytes([0x90, 0x3C, 0x7F])
    interval = args.interval_ms / 1e3
    print(f"jitter on {rp.hwname}: {args.count} msgs at {args.interval_ms} ms "
          f"interval\n")
    arrivals = []
    rp.drain_input(0.05)
    base = time.perf_counter()
    for i in range(args.count):
        target = base + i * interval
        now = time.perf_counter()
        if target > now:
            time.sleep(target - now)
        rp.write(msg)
        data, t_first, t_last = rp.collect(3, 0.5)
        if t_first is not None:
            arrivals.append(t_first)
    deltas = [(arrivals[i] - arrivals[i - 1]) * 1e3
              for i in range(1, len(arrivals))]
    _stats("received inter-arrival", deltas)
    if deltas:
        jit = [abs(d - args.interval_ms) for d in deltas]
        _stats("abs jitter vs target", jit)
    print(f"  received {len(arrivals)}/{args.count}")
    rp.close()
    return 0


def cmd_throughput(args):
    rp = open_port(args)
    msg = bytes([0x90, 0x3C, 0x7F])
    rate_s = "as fast as possible" if not args.rate else f"{args.rate:.0f} msg/s"
    print(f"throughput on {rp.hwname}: {rate_s} for {args.duration}s, "
          f"counting echoes\n")
    interval = 1.0 / args.rate if args.rate else 0
    rp.drain_input(0.05)
    sent = 0
    rxbuf = bytearray()
    base = time.perf_counter()
    t_end = base + args.duration
    while time.perf_counter() < t_end:
        if interval:
            target = base + sent * interval
            now = time.perf_counter()
            if target > now:
                time.sleep(target - now)
        rp.write(msg)                           # write+drain backpressures at limit
        sent += 1
        rxbuf += rp.read_ready()                # drain echoes as we go
    for _, c in rp.collect_window(0.5):         # collect the tail
        rxbuf += c
    recv = len(canonicalize(bytes(rxbuf)))
    loss = sent - recv
    print(f"  attempted {sent} msg = {sent/args.duration:.0f} msg/s")
    print(f"  echoed back {recv}   loss {loss} "
          f"({100*loss/max(sent,1):.1f}%)")
    print(f"  UART ceiling ~ {31250/10/3:.0f} 3-byte msg/s per port "
          "(31250 baud / 10 bits / 3 bytes)")
    print("  (caveat: at the limit, some 'loss' can be host input-buffer "
          "overflow rather than device drops; sweep --rate to find the knee.)")
    rp.close()
    return 0 if loss == 0 else 1


def cmd_soak(args):
    rp = open_port(args)
    msg = bytes([0x90, 0x3C, 0x7F])
    n = args.count
    print(f"soak on {rp.hwname}: {n} round-trips, counting errors\n")
    lats, drops, corrupt = _latency_samples(rp, n, msg, gap=args.gap)
    _stats("round-trip latency", lats)
    print(f"  drops {drops}  corrupt {corrupt}  ok {len(lats)}/{n}")
    rp.close()
    return 0 if drops == 0 and corrupt == 0 else 1


def cmd_isolation(args):
    nums = [int(x) for x in args.ports.split(",")]
    ports = {n: open_port(args, n) for n in nums}
    print(f"isolation: for each port, send and confirm only that port echoes\n")
    ok_all = True
    for src in nums:
        for p in ports.values():
            p.drain_input(0.02)
        msg = bytes([0x90, 0x3C + (src & 0x0F), 0x7F])
        ports[src].write(msg)
        time.sleep(0.2)
        leaked = []
        self_ok = False
        for n, p in ports.items():
            got = canonicalize(p.read_ready())
            if n == src:
                self_ok = got == canonicalize(msg)
            elif got:
                leaked.append((n, got))
        ok = self_ok and not leaked
        ok_all &= ok
        info = "own echo OK" if self_ok else "OWN ECHO MISSING"
        if leaked:
            info += "  LEAKED to " + ", ".join(f"port{n}:{hxlist(g)}"
                                                for n, g in leaked)
        print(f"  send on port {src}: {'OK' if ok else 'FAIL'}  ({info})")
    for p in ports.values():
        p.close()
    return 0 if ok_all else 1


def cmd_sysexsweep(args):
    """Send SysEx of increasing length and report how much round-trips, to find
    where (and how) long SysEx starts failing. With --rxport, send on -p and
    receive on --rxport (cross cable: <p> DIN OUT -> <rxport> DIN IN), which
    distinguishes a same-channel self-loopback artifact from a firmware bug."""
    tx = open_port(args)
    rx = open_port(args, args.rxport) if args.rxport else tx
    where = f"{tx.hwname} -> {rx.hwname}" if args.rxport else tx.hwname
    print(f"sysex length sweep on {where}:\n")
    for nd in [1, 5, 10, 15, 20, 30, 40, 60, 80, 100, 120, 130, 150, 200]:
        msg = bytes([0xF0, 0x7D]) + bytes((i & 0x7F) for i in range(nd)) \
            + bytes([0xF7])
        rx.drain_input(0.05)
        tx.write(msg)
        data, _, _ = rx.collect_idle(0.04, 2.0)
        f0 = (data[:1] == b"\xF0")
        ok = data == msg
        print(f"  data={nd:3d}  sent={len(msg):3d}  recv={len(data):3d}  "
              f"F0={'y' if f0 else 'n'}  {'OK' if ok else 'FAIL'}  "
              f"recv[:6]=[{hx(data[:6])}]  recv[-3:]=[{hx(data[-3:])}]")
    tx.close()
    if args.rxport:
        rx.close()
    return 0


def _classify_corrupt(sent, got):
    """Classify how a received SysEx echo differs from what was sent, to tell a
    firmware byte-drop (interior byte missing, F7 still present) from a host-side
    truncation (got is a prefix, no F7) from a substitution/cross-talk."""
    if len(got) == 0:
        return "EMPTY (no bytes parsed)"
    f7 = got[-1] == 0xF7
    if got == sent:
        return "identical?! (canonicalize artifact)"
    if len(got) < len(sent) and sent.startswith(got):
        return (f"TRUNCATED prefix: got {len(got)}/{len(sent)} B, "
                f"endF7={f7}, tail=[{hx(got[-4:])}]")
    # interior single-byte deletion?
    for i in range(min(len(sent), len(got) + 1)):
        if sent[:i] + sent[i + 1:] == got:
            return (f"INTERIOR DROP @idx {i}: removed {sent[i]:#04x} "
                    f"(ctx sent=[{hx(sent[max(0,i-2):i+3])}]) endF7={f7}")
    # first divergence
    n = min(len(sent), len(got))
    div = next((i for i in range(n) if sent[i] != got[i]), n)
    return (f"DIVERGE @idx {div}: sent {len(sent)}B got {len(got)}B endF7={f7} "
            f"sent[{div}:]=[{hx(sent[div:div+5])}] got[{div}:]=[{hx(got[div:div+5])}]")


def cmd_sysexsoak(args):
    """Sustained long SysEx on N ports simultaneously -- the regression test for
    the timer-ISR RX fix (the FIFO-less ST16C454 dropping a byte on long
    streams). Drives all ports at once, compares canonicalized echoes, and reads
    the firmware RX-overflow counter (0 = no chip overrun). Needs a loopback
    cable on each tested port."""
    port_nums = [int(x) for x in args.ports.split(",")]
    rps = [open_port(args, port=p) for p in port_nums]
    payload = bytes((i & 0x7F) for i in range(args.size))
    msg = bytes([0xF0, 0x7D]) + payload + bytes([0xF7])     # one long SysEx
    exp = canonicalize(msg)

    clear_rx_overflows()
    for rp in rps:
        rp.drain_input(0.1)        # clear startup/stale bytes before measuring
    drops = corrupt = 0
    samples = []
    print(f"sysexsoak: {args.reps} reps of a {len(msg)}B SysEx on ports "
          f"{port_nums} (loop DIN OUT->IN on each) ...")
    for rep in range(args.reps):
        for rp in rps:
            rp.drain_input(0.01)   # drop any inter-rep residue (echo is complete)
        for rp in rps:
            rp.write(msg)
        for i, rp in enumerate(rps):
            data, _t_first, t_last = rp.collect_idle(0.05, 3.0)
            if t_last is None:
                drops += 1
            elif canonicalize(data) != exp:
                corrupt += 1
                if getattr(args, "debug", False) and len(samples) < 20:
                    samples.append((rep, port_nums[i],
                                    _classify_corrupt(msg, data)))
    for rp in rps:
        rp.close()

    if samples:
        print("  corrupt samples (rep, port, classification):")
        for rep, port, info in samples:
            print(f"    rep {rep:4d} port {port}: {info}")

    ovf = read_rx_overflows()
    print(f"  drops {drops}  corrupt {corrupt}")
    print(f"  fw RX-overflow counter: "
          f"{'(unavailable)' if ovf is None else ovf}")
    ok = drops == 0 and corrupt == 0 and ovf in (0, None)
    print(f"=> {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def cmd_selftest(args):
    """Validate the canonicalizer without hardware."""
    cases = [
        (b"\x90\x3C\x7F", [b"\x90\x3C\x7F"]),
        (b"\x90\x3C\x7F\x3E\x7F\x40\x7F",
         [b"\x90\x3C\x7F", b"\x90\x3E\x7F", b"\x90\x40\x7F"]),    # running status
        (b"\xC0\x05\x06\x07", [b"\xC0\x05", b"\xC0\x06", b"\xC0\x07"]),
        (b"\xF0\x7D\x11\xF7", [b"\xF0\x7D\x11\xF7"]),             # sysex
        (b"\x90\x3C\xF8\x7F", [b"\x90\x3C\x7F"]),                 # RT mid-msg dropped
        (b"\xF0\x11\xF8\x22\xF7", [b"\xF0\x11\x22\xF7"]),         # RT mid-sysex dropped
        (b"\xF2\x12\x34", [b"\xF2\x12\x34"]),                     # song position
        (b"\xF6", [b"\xF6"]),                                     # tune request
        (b"\x05\x06\x90\x3C\x7F", [b"\x90\x3C\x7F"]),             # orphan data dropped
        (b"\x90\x3C\xF8\xF8\x7F\x3E\x7F",
         [b"\x90\x3C\x7F", b"\x90\x3E\x7F"]),       # RT doesn't break running status
    ]
    npass = 0
    for data, exp in cases:
        got = canonicalize(data)
        ok = got == exp
        npass += ok
        print(f"  {'OK' if ok else 'FAIL'}  in [{hx(data)}] -> {hxlist(got)}"
              + ("" if ok else f"   expected {hxlist(exp)}"))
    # keep-realtime variant: the F8 arrives mid-message, so it is emitted
    # BEFORE the note it interrupted completes.
    got = canonicalize(b"\x90\x3C\xF8\x7F", drop_realtime=False)
    exp = [b"\xF8", b"\x90\x3C\x7F"]
    ok = got == exp
    npass += ok
    print(f"  {'OK' if ok else 'FAIL'}  keep-RT [90 3C F8 7F] -> {hxlist(got)}")
    total = len(cases) + 1
    print(f"\n=> {npass}/{total} canonicalizer checks passed.")
    return 0 if npass == total else 1


def cmd_all(args):
    rc = 0
    for fn in (cmd_functional, cmd_sysex):
        rc |= fn(args)
        print()
    args.count = min(args.count, 500)
    rc |= cmd_timing(args)
    return rc


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", type=int, default=1,
                    help="MIDEX port number (1-based, default 1)")
    ap.add_argument("-m", "--match", default="MIDEX8 UAC",
                    help="port-name substring (default 'MIDEX8 UAC' for the "
                         "class firmware; use 'MIDEX Port' for the stock "
                         "snd-usb-midex firmware)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("functional").set_defaults(fn=cmd_functional)
    sub.add_parser("sysex").set_defaults(fn=cmd_sysex)
    t = sub.add_parser("timing"); t.add_argument("--count", type=int, default=1000)
    t.set_defaults(fn=cmd_timing)
    j = sub.add_parser("jitter")
    j.add_argument("--count", type=int, default=1000)
    j.add_argument("--interval-ms", type=float, default=10.0)
    j.set_defaults(fn=cmd_jitter)
    th = sub.add_parser("throughput")
    th.add_argument("--duration", type=float, default=5.0)
    th.add_argument("--rate", type=float, default=0,
                    help="target msg/s (0 = as fast as the device accepts)")
    th.set_defaults(fn=cmd_throughput)
    s = sub.add_parser("soak")
    s.add_argument("--count", type=int, default=50000)
    s.add_argument("--gap", type=float, default=0.0)
    s.set_defaults(fn=cmd_soak)
    iso = sub.add_parser("isolation")
    iso.add_argument("--ports", default="1,2,3")
    iso.set_defaults(fn=cmd_isolation)
    sw = sub.add_parser("sysexsweep")
    sw.add_argument("--rxport", type=int, default=0,
                    help="receive on a different port (cross cable) to test "
                         "self-loopback vs firmware")
    sw.set_defaults(fn=cmd_sysexsweep)
    so = sub.add_parser("sysexsoak")
    so.add_argument("--ports", default="1,2,3",
                    help="comma-separated ports to drive at once")
    so.add_argument("--reps", type=int, default=200)
    so.add_argument("--size", type=int, default=256,
                    help="SysEx payload size in bytes")
    so.add_argument("--debug", action="store_true",
                    help="classify each corrupt echo (drop vs truncation)")
    so.set_defaults(fn=cmd_sysexsoak)
    sub.add_parser("selftest").set_defaults(fn=cmd_selftest)
    a = sub.add_parser("all"); a.add_argument("--count", type=int, default=500)
    a.set_defaults(fn=cmd_all)
    args = ap.parse_args()
    sys.exit(args.fn(args))


if __name__ == "__main__":
    main()
