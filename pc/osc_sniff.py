"""Listens for OSC on a UDP port and prints each address once, with its value range.
Used to see what EyeTrackVR / Baballonia actually send.

  python osc_sniff.py 9010 20      (port, seconds)
"""

import socket
import struct
import sys
import time


def read_str(data, i):
    end = data.index(b"\0", i)
    s = data[i:end].decode("ascii", "replace")
    return s, (end + 4) & ~3


def parse(data, out):
    if data.startswith(b"#bundle"):
        i = 16
        while i + 4 <= len(data):
            n = struct.unpack(">i", data[i:i + 4])[0]
            parse(data[i + 4:i + 4 + n], out)
            i += 4 + n
        return
    addr, i = read_str(data, 0)
    tags, i = read_str(data, i)
    vals = []
    for t in tags[1:]:
        if t == "f":
            vals.append(struct.unpack(">f", data[i:i + 4])[0]); i += 4
        elif t == "i":
            vals.append(struct.unpack(">i", data[i:i + 4])[0]); i += 4
        elif t == "s":
            s, i = read_str(data, i); vals.append(s)
        elif t in "TF":
            vals.append(t == "T")
    out.append((addr, vals))


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9010
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", port))
    sock.settimeout(0.5)
    stats = {}
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            data, _ = sock.recvfrom(65536)
        except socket.timeout:
            continue
        msgs = []
        try:
            parse(data, msgs)
        except Exception:
            continue
        for addr, vals in msgs:
            s = stats.setdefault(addr, {"n": 0, "min": None, "max": None, "last": None})
            s["n"] += 1
            s["last"] = vals
            if vals and isinstance(vals[0], (int, float)) and not isinstance(vals[0], bool):
                v = vals[0]
                s["min"] = v if s["min"] is None else min(s["min"], v)
                s["max"] = v if s["max"] is None else max(s["max"], v)
    if not stats:
        print(f"nothing received on 127.0.0.1:{port}")
    for addr, s in sorted(stats.items()):
        print(f"{addr:50s} n={s['n']:5d} min={s['min']} max={s['max']} last={s['last']}")


if __name__ == "__main__":
    main()
