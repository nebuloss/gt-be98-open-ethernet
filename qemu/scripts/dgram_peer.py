#!/usr/bin/env python3
# QEMU -netdev dgram peer: each UDP datagram payload IS one raw ethernet frame.
# Usage: dgram_peer.py <listen_port> <send_port> <mode>
#   mode capture : print every frame received from the guest (TX proof), pcap.
#   mode inject  : after 'sleep', send N test frames into the guest (RX proof).
import socket, sys, time, struct, binascii, threading, os

listen_port = int(sys.argv[1])   # we receive guest TX here  (QEMU remote.port)
send_port   = int(sys.argv[2])   # we inject guest RX here    (QEMU local.port)
mode        = sys.argv[3]
pcap_path   = sys.argv[4] if len(sys.argv) > 4 else None

rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
rx.bind(("127.0.0.1", listen_port))
rx.settimeout(0.5)

tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

pcap = open(pcap_path, "wb") if pcap_path else None
if pcap:
    # pcap global header, linktype 1 = Ethernet
    pcap.write(struct.pack("<IHHiIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
    pcap.flush()

def log(*a):
    print("[peer]", *a, flush=True)

def build_frame(dst, src, ethertype, payload):
    return dst + src + struct.pack("!H", ethertype) + payload

GUEST_MAC = bytes.fromhex("020000000002")   # arbitrary guest-side dst
PEER_MAC  = bytes.fromhex("020000000001")

captured = 0
def capture_loop(deadline):
    global captured
    while time.time() < deadline:
        try:
            data, addr = rx.recvfrom(65535)
        except socket.timeout:
            continue
        captured += 1
        log("RX-from-guest len=%d  %s" % (len(data), binascii.hexlify(data[:32]).decode()))
        if pcap:
            ts = time.time()
            pcap.write(struct.pack("<IIII", int(ts), int((ts%1)*1e6), len(data), len(data)))
            pcap.write(data); pcap.flush()

if mode == "capture":
    dur = float(os.environ.get("PEER_DUR", "55"))
    capture_loop(time.time()+dur)
    log("TOTAL captured frames from guest:", captured)

elif mode == "inject":
    # run a capture thread too (so we also see guest TX), then inject after delay
    dur = float(os.environ.get("PEER_DUR", "55"))
    deadline = time.time()+dur
    t = threading.Thread(target=capture_loop, args=(deadline,), daemon=True)
    t.start()
    delay = float(os.environ.get("INJECT_DELAY", "20"))
    nframes = int(os.environ.get("INJECT_N", "5"))
    log("inject: sleeping %ds before injecting %d frames" % (delay, nframes))
    time.sleep(delay)
    for i in range(nframes):
        payload = ("BE98-RX-TEST-%03d-" % i).encode() + b"\xAB"*40
        frame = build_frame(b"\xff"*6, PEER_MAC, 0x88b5, payload)  # broadcast, local-exp ethertype
        tx.sendto(frame, ("127.0.0.1", send_port))
        log("injected frame %d len=%d" % (i, len(frame)))
        time.sleep(0.3)
    # keep capturing until deadline
    while time.time() < deadline:
        time.sleep(0.5)
    log("TOTAL captured frames from guest:", captured)

if pcap: pcap.close()
