"""PC receiver for PicoEyeStreamer (Pico Neo 3 Pro Eye).

Listens for the headset's UDP packets, shows a live readout, and optionally:
  --csv FILE   log every packet to CSV
  --osc        forward to VRChat's native eye tracking OSC (127.0.0.1:9000)

No third-party packages needed.

Examples:
  python pico_eye_receiver.py
  python pico_eye_receiver.py --csv eyes.csv
  python pico_eye_receiver.py --osc
"""

import argparse
import csv
import math
import socket
import struct
import sys
import time

FIELDS = ["seq", "time_s", "ok", "comb_status", "l_status", "r_status",
          "gx", "gy", "gz", "px", "py", "pz", "l_open", "r_open"]


def osc_message(address, *floats):
    def pad(b):
        return b + b"\0" * (4 - len(b) % 4)
    return (pad(address.encode()) + pad(("," + "f" * len(floats)).encode())
            + b"".join(struct.pack(">f", f) for f in floats))


def parse(data):
    parts = data.decode("ascii", "replace").strip().split(",")
    if len(parts) != len(FIELDS) + 1 or parts[0] != "PEYE":
        return None
    v = parts[1:]
    try:
        return {
            "seq": int(v[0]), "time_s": float(v[1]), "ok": v[2] == "1",
            "comb_status": int(v[3]), "l_status": int(v[4]), "r_status": int(v[5]),
            "gx": float(v[6]), "gy": float(v[7]), "gz": float(v[8]),
            "px": float(v[9]), "py": float(v[10]), "pz": float(v[11]),
            "l_open": float(v[12]), "r_open": float(v[13]),
        }
    except ValueError:
        return None


def gaze_to_pitch_yaw(gx, gy, gz):
    """Unity space: +x right, +y up, +z forward. Returns degrees; pitch + = looking down."""
    n = math.sqrt(gx * gx + gy * gy + gz * gz)
    if n < 1e-6:
        return 0.0, 0.0
    gx, gy, gz = gx / n, gy / n, gz / n
    yaw = math.degrees(math.atan2(gx, gz))
    pitch = -math.degrees(math.asin(max(-1.0, min(1.0, gy))))
    return pitch, yaw


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=7777, help="UDP port to listen on (default 7777)")
    ap.add_argument("--csv", help="write packets to this CSV file")
    ap.add_argument("--osc", action="store_true", help="forward to VRChat eye tracking OSC")
    ap.add_argument("--osc-host", default="127.0.0.1")
    ap.add_argument("--osc-port", type=int, default=9000)
    ap.add_argument("--invert-pitch", action="store_true", help="flip up/down if eyes move the wrong way")
    ap.add_argument("--invert-yaw", action="store_true", help="flip left/right if eyes move the wrong way")
    ap.add_argument("--gain", type=float, default=1.0, help="multiply gaze angles (default 1.0)")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.port))
    sock.settimeout(1.0)

    osc_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM) if args.osc else None
    osc_dest = (args.osc_host, args.osc_port)

    csv_file = writer = None
    if args.csv:
        csv_file = open(args.csv, "w", newline="", encoding="utf-8")
        writer = csv.writer(csv_file)
        writer.writerow(["pc_time"] + FIELDS + ["pitch_deg", "yaw_deg"])

    print(f"Listening on UDP {args.port}" + (f", OSC -> {osc_dest}" if args.osc else "")
          + (f", CSV -> {args.csv}" if args.csv else ""))
    print("If nothing arrives: allow Python through Windows Firewall (private network) "
          "and check the headset is on the same Wi-Fi.")

    last_print = time.time()
    count = valid = lost = 0
    last_seq = None
    sender = None
    pitch = yaw = 0.0
    s = None

    try:
        while True:
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                if time.time() - last_print > 2:
                    print("... waiting for headset packets")
                    last_print = time.time()
                continue

            s = parse(data)
            if s is None:
                continue
            if sender != addr[0]:
                sender = addr[0]
                print(f"\nReceiving from {sender}")
            if last_seq is not None and s["seq"] > last_seq + 1:
                lost += s["seq"] - last_seq - 1
            last_seq = s["seq"]
            count += 1

            if s["ok"]:
                valid += 1
                pitch, yaw = gaze_to_pitch_yaw(s["gx"], s["gy"], s["gz"])
                pitch *= args.gain * (-1 if args.invert_pitch else 1)
                yaw *= args.gain * (-1 if args.invert_yaw else 1)

            if writer:
                writer.writerow([f"{time.time():.4f}"] + [s[k] for k in FIELDS]
                                + [f"{pitch:.3f}", f"{yaw:.3f}"])

            if osc_sock:
                if s["ok"]:
                    osc_sock.sendto(osc_message("/tracking/eye/CenterPitchYaw", pitch, yaw), osc_dest)
                opens = [o for o in (s["l_open"], s["r_open"]) if o >= 0]
                if opens:
                    closed = 1.0 - max(0.0, min(1.0, sum(opens) / len(opens)))
                    osc_sock.sendto(osc_message("/tracking/eye/EyesClosedAmount", closed), osc_dest)

            now = time.time()
            if now - last_print >= 1.0:
                rate = count / (now - last_print)
                sys.stdout.write(
                    f"\r{rate:5.0f} Hz  valid {valid:3d}  lost {lost:4d}  "
                    f"pitch {pitch:+6.1f}  yaw {yaw:+6.1f}  "
                    f"open L {s['l_open']:.2f} R {s['r_open']:.2f}  "
                    f"status c{s['comb_status']} l{s['l_status']} r{s['r_status']}   ")
                sys.stdout.flush()
                if csv_file:
                    csv_file.flush()
                count = valid = 0
                last_print = now
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        if csv_file:
            csv_file.close()


if __name__ == "__main__":
    main()
