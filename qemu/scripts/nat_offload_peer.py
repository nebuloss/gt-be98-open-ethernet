#!/usr/bin/env python3
"""
NAT-offload QEMU test peer (Phase 2).

Drives the routed-L3 + SNAT/NAPT HW-offload proof for the open BCM4916 Runner
driver against the QEMU Runner model. Talks the dgram netdev backend (each UDP
datagram = one raw Ethernet frame).

It injects real IPv4/TCP frames whose ORIGINAL 5-tuple matches the flow the
driver programs into NAT-C via the offload_nat_selftest debugfs trigger:

    original  src 192.0.2.10:4096  ->  dst 198.51.100.20:80   (TCP, TTL 64)
    SNAT/NAPT src 203.0.113.5:5000 (post-translation, applied by the cmdlist)

Phase A (pre-program): frames MISS NAT-C -> CPU slow path (no rewrite).
Phase B (post-program): frames HIT NAT-C -> the Runner model runs the NAT
cmdlist (dec-TTL, SNAT IP, SNAPT port, IP+TCP csum) and forwards the rewritten
frame to this peer, CPU bypassed.

The peer captures the forwarded frames, parses the rewritten 5-tuple, and
VERIFIES: src IP rewritten to 203.0.113.5, src port to 5000, TTL decremented to
63, and the IP + TCP checksums correct. (Addresses are TEST-NET/documentation
ranges - public-safe.)

usage: nat_offload_peer.py <listen_port> <send_port> [pcap]
"""
import socket, sys, time, struct, os, threading

listen_port = int(sys.argv[1]); send_port = int(sys.argv[2])
pcap_path = sys.argv[3] if len(sys.argv) > 3 else None

rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
rx.bind(("127.0.0.1", listen_port)); rx.settimeout(0.5)
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
pcap = open(pcap_path, "wb") if pcap_path else None
if pcap:
    pcap.write(struct.pack("<IHHiIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1)); pcap.flush()


def log(*a):
    print("[nat-peer]", *a, flush=True)


PEER_MAC = bytes.fromhex("020000000001")
DST_MAC = bytes.fromhex("020000000002")     # the runner NIC MAC (any; L3 key ignores MAC)

ORIG_SIP = "192.0.2.10"
ORIG_DIP = "198.51.100.20"
ORIG_SPORT = 4096
ORIG_DPORT = 80
NAT_SIP = "203.0.113.5"
NAT_SPORT = 5000


def ip2b(s):
    return socket.inet_aton(s)


def csum16(data):
    if len(data) % 2:
        data += b"\x00"
    s = 0
    for i in range(0, len(data), 2):
        s += (data[i] << 8) | data[i + 1]
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff


def build_ipv4_tcp(seq):
    """Build Eth/IPv4/TCP frame with the ORIGINAL 5-tuple + a small payload."""
    payload = b"NATOFF%03d" % (seq % 1000) + b"\xCD" * 8

    # TCP header (20 bytes, no options)
    sport, dport = ORIG_SPORT, ORIG_DPORT
    seqno, ackno = 0x11223344, 0
    off_flags = (5 << 12) | 0x10   # data offset 5 words, ACK
    win, urg = 8192, 0
    tcp_wo_csum = struct.pack("!HHIIHHHH", sport, dport, seqno, ackno,
                              off_flags, win, 0, urg) + payload
    # TCP pseudo-header for checksum
    pseudo = ip2b(ORIG_SIP) + ip2b(ORIG_DIP) + struct.pack("!BBH", 0, 6, len(tcp_wo_csum))
    tcp_csum = csum16(pseudo + tcp_wo_csum)
    tcp = struct.pack("!HHIIHHHH", sport, dport, seqno, ackno,
                      off_flags, win, tcp_csum, urg) + payload

    # IPv4 header (20 bytes, IHL=5, TTL=64)
    total_len = 20 + len(tcp)
    ihl_ver = 0x45
    ip_wo_csum = struct.pack("!BBHHHBBH", ihl_ver, 0, total_len, seq & 0xffff,
                             0, 64, 6, 0) + ip2b(ORIG_SIP) + ip2b(ORIG_DIP)
    ip_csum = csum16(ip_wo_csum)
    ip = struct.pack("!BBHHHBBH", ihl_ver, 0, total_len, seq & 0xffff,
                     0, 64, 6, ip_csum) + ip2b(ORIG_SIP) + ip2b(ORIG_DIP)

    return DST_MAC + PEER_MAC + struct.pack("!H", 0x0800) + ip + tcp


def parse_and_verify(frame):
    """Parse a (possibly NAT-rewritten) Eth/IPv4/TCP frame; return a dict."""
    if len(frame) < 14 + 20 + 20:
        return None
    if frame[12:14] != b"\x08\x00":
        return None
    ip = frame[14:]
    ihl = (ip[0] & 0x0f) * 4
    ttl = ip[8]
    ip_csum_field = (ip[10] << 8) | ip[11]
    sip = socket.inet_ntoa(ip[12:16])
    dip = socket.inet_ntoa(ip[16:20])
    l4 = ip[ihl:]
    sport = (l4[0] << 8) | l4[1]
    dport = (l4[2] << 8) | l4[3]
    tcp_csum_field = (l4[16] << 8) | l4[17]

    # recompute IP csum (over header with csum field zeroed)
    iphdr = bytearray(ip[:ihl]); iphdr[10] = iphdr[11] = 0
    ip_csum_ok = (csum16(bytes(iphdr)) == ip_csum_field)

    # recompute TCP csum
    l4buf = bytearray(l4); l4buf[16] = l4buf[17] = 0
    pseudo = ip[12:16] + ip[16:20] + struct.pack("!BBH", 0, ip[9], len(l4))
    tcp_csum_ok = (csum16(pseudo + bytes(l4buf)) == tcp_csum_field)

    return dict(sip=sip, dip=dip, sport=sport, dport=dport, ttl=ttl,
                ip_csum_ok=ip_csum_ok, tcp_csum_ok=tcp_csum_ok)


cap = {"n": 0, "fwd": 0, "verified": 0}


def caploop(deadline):
    while time.time() < deadline:
        try:
            data, _ = rx.recvfrom(65535)
        except socket.timeout:
            continue
        cap["n"] += 1
        if pcap:
            ts = time.time()
            pcap.write(struct.pack("<IIII", int(ts), int((ts % 1) * 1e6),
                                   len(data), len(data)))
            pcap.write(data); pcap.flush()
        info = parse_and_verify(data)
        if not info:
            log("RX-from-guest len=%d (non-IPv4/short)" % len(data))
            continue
        cap["fwd"] += 1
        ok = (info["sip"] == NAT_SIP and info["sport"] == NAT_SPORT and
              info["ttl"] == 63 and info["ip_csum_ok"] and info["tcp_csum_ok"])
        if ok:
            cap["verified"] += 1
        log("RX-from-guest len=%d  src=%s:%d dst=%s:%d ttl=%d ipcsum=%s tcpcsum=%s  %s"
            % (len(data), info["sip"], info["sport"], info["dip"], info["dport"],
               info["ttl"], "OK" if info["ip_csum_ok"] else "BAD",
               "OK" if info["tcp_csum_ok"] else "BAD",
               "NAT-VERIFIED" if ok else "NOT-REWRITTEN"))


dur = float(os.environ.get("PEER_DUR", "40"))
deadline = time.time() + dur
threading.Thread(target=caploop, args=(deadline,), daemon=True).start()

# prime the QEMU dgram path early
time.sleep(1); tx.sendto(build_ipv4_tcp(0), ("127.0.0.1", send_port)); log("primed")

# PHASE A: pre-program MISS frames ~t=4s
time.sleep(3); log("=== PHASE A: 4 MISS frames (pre-program, expect slow path) ===")
for i in range(4):
    tx.sendto(build_ipv4_tcp(100 + i), ("127.0.0.1", send_port))
    log("inj MISS%d" % i); time.sleep(0.5)

# PHASE B: post-program HIT frames ~t=12s (init programs NAT-C ~t=10s)
time.sleep(7); log("=== PHASE B: 6 HIT frames (post-program, expect NAT rewrite+fwd) ===")
for i in range(6):
    tx.sendto(build_ipv4_tcp(200 + i), ("127.0.0.1", send_port))
    log("inj HIT%d" % i); time.sleep(0.6)

while time.time() < deadline:
    time.sleep(0.5)

log("TOTAL from guest=%d  IPv4-forwarded=%d  NAT-VERIFIED=%d"
    % (cap["n"], cap["fwd"], cap["verified"]))
if pcap:
    pcap.close()
