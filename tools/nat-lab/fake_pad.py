#!/usr/bin/env python3
"""fake_pad: minimal WUP Stream client for the isolated NAT lab.

Speaks the real wire protocol against lab-server/wup-thinserver:
  B beacon -> H hellos -> I input reports -> V/A stream -> N retransmit
  requests -> T mode requests.

Prints one RESULT key=value line at the end for the shell harness.
Exit code 0 on success, 1 on protocol/timeout failure.
"""
import argparse
import socket
import struct
import sys
import time

I_PERIOD = 0.02          # ~50 Hz input reports (well above the 2 s watchdog)
TICKS = 16666            # us per 59.94 Hz refresh


def build_i(tick_acc):
    # 'I' flags u16 ticks u8 backlight 0 0 0 + 128-byte report = 136 bytes
    tick_acc = (tick_acc + TICKS) % 65536
    return (b"I" + b"\x00" + struct.pack("<H", tick_acc) + b"\x05"
            + b"\x00" * 3 + bytes(128)), tick_acc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True)
    ap.add_argument("--port", type=int, default=50001)
    ap.add_argument("--sport", type=int, default=50000)
    ap.add_argument("--seconds", type=float, default=4.0)
    ap.add_argument("--beacons", type=int, default=30,
                    help="max B retries while waiting for H")
    ap.add_argument("--send-t", action="store_true",
                    help="send a T mode request mid-session")
    ap.add_argument("--send-n", action="store_true",
                    help="send an N retransmit request mid-session")
    ap.add_argument("--peer-port", type=int, default=0,
                    help="if set, require the H source port to equal this")
    ap.add_argument("--probe", action="store_true",
                    help="exit 0 once a valid H arrives (no media required)")
    ap.add_argument("--spray", action="store_true",
                    help="also send I reports while beaconing (no H needed)")
    ap.add_argument("--keyframe", action="store_true",
                    help="set the I keyframe-request flag")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.sport))
    sock.settimeout(0.5)
    server = (args.server, args.port)

    stats = dict(hellos=0, videos=0, audios=0, t_acks=0, rumbles=0,
                 quits=0, unknown=0, peer="", frames=set())
    b = b"B\x01\x00\x00"

    # --- beacon until H -----------------------------------------------------
    deadline = time.time() + args.beacons * 0.5
    hello = None
    tick = 0
    while time.time() < deadline:
        sock.sendto(b, server)
        if args.spray:
            pkt, tick = build_i(tick)
            sock.sendto(pkt, server)
        try:
            data, peer = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if data[:1] == b"H":
            hello = (data, peer)
            break
    if hello is None:
        print("RESULT ok=0 stage=hello")
        return 1
    data, peer = hello
    stats["hellos"] = 1
    stats["peer"] = f"{peer[0]}:{peer[1]}"
    if args.peer_port and peer[1] != args.peer_port:
        print(f"RESULT ok=0 stage=peerport expected={args.peer_port} "
              f"got={peer[1]}")
        return 1
    if args.probe:
        print(f"RESULT ok=1 probe=1 peer={stats['peer']}")
        return 0

    # --- session ------------------------------------------------------------
    tick = 0
    start = time.time()
    end = start + args.seconds
    next_i = start
    t_sent = False
    n_sent = False
    while time.time() < end:
        now = time.time()
        while now >= next_i:
            flags = b"\x01" if args.keyframe else b"\x00"
            pkt, tick = build_i(tick)
            if args.keyframe:
                pkt = b"I" + flags + pkt[2:]
            sock.sendto(pkt, server)
            next_i += I_PERIOD
        try:
            data, _ = sock.recvfrom(65535)
        except socket.timeout:
            continue
        tag = data[:1]
        if tag == b"H":
            stats["hellos"] += 1
        elif tag == b"V":
            stats["videos"] += 1
            if len(data) >= 16:
                stats["frames"].add(struct.unpack("<I", data[4:8])[0])
        elif tag == b"A":
            stats["audios"] += 1
        elif tag == b"T":
            stats["t_acks"] += 1
        elif tag == b"R":
            stats["rumbles"] += 1
        elif tag == b"Q":
            stats["quits"] += 1
        else:
            stats["unknown"] += 1
        if args.send_t and not t_sent and now - start > 1.0:
            sock.sendto(b"T\x07\x01\x00", server)   # reqid 7, mode Desktop
            t_sent = True
        if args.send_n and not n_sent and stats["videos"] > 20:
            # N count=1, frame 0 (likely absent -> harmless), packet 0xffff
            sock.sendto(b"N\x01\x00\x00" + struct.pack("<IHH", 0, 0xFFFF, 0),
                        server)
            n_sent = True

    ok = stats["videos"] > 0 and stats["audios"] > 0
    if args.send_t:
        ok = ok and stats["t_acks"] > 0
    print(f"RESULT ok={1 if ok else 0} hellos={stats['hellos']} "
          f"videos={stats['videos']} uniq_frames={len(stats['frames'])} "
          f"audios={stats['audios']} t_acks={stats['t_acks']} "
          f"quits={stats['quits']} peer={stats['peer']}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
