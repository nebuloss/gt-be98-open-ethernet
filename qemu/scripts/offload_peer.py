#!/usr/bin/env python3
import socket, sys, time, struct, binascii, threading, os
listen_port=int(sys.argv[1]); send_port=int(sys.argv[2]); pcap_path=sys.argv[3] if len(sys.argv)>3 else None
rx=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); rx.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
rx.bind(("127.0.0.1",listen_port)); rx.settimeout(0.5)
tx=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
pcap=open(pcap_path,"wb") if pcap_path else None
if pcap: pcap.write(struct.pack("<IHHiIII",0xa1b2c3d4,2,4,0,0,65535,1)); pcap.flush()
def log(*a): print("[peer]",*a,flush=True)
PEER_MAC=bytes.fromhex("020000000001")
def mkframe(tag,i):
    payload=("%s%03d"%(tag,i)).encode()+b"\xAB"*40
    return b"\xff"*6+PEER_MAC+struct.pack("!H",0x0800)+payload
cap={"n":0,"fwd":0}
def caploop(deadline):
    while time.time()<deadline:
        try: data,_=rx.recvfrom(65535)
        except socket.timeout: continue
        cap["n"]+=1
        if b"HIT" in data: cap["fwd"]+=1
        tagpart=data[14:21] if len(data)>=21 else b""
        log("RX-from-guest len=%d tag=%s"%(len(data),tagpart))
        if pcap:
            ts=time.time(); pcap.write(struct.pack("<IIII",int(ts),int((ts%1)*1e6),len(data),len(data))); pcap.write(data); pcap.flush()
dur=float(os.environ.get("PEER_DUR","32")); deadline=time.time()+dur
threading.Thread(target=caploop,args=(deadline,),daemon=True).start()
# prime the QEMU dgram path early
time.sleep(1); tx.sendto(mkframe("PRIME",0),("127.0.0.1",send_port)); log("primed")
# PHASE 1 pre-program MISS frames ~t=4s
time.sleep(3); log("=== PHASE1: 4 MISS frames (pre-program) ===")
for i in range(4): tx.sendto(mkframe("MISS",i),("127.0.0.1",send_port)); log("inj MISS%d"%i); time.sleep(0.5)
# PHASE 2 post-program HIT frames ~t=12s (init programs ~t=10s)
time.sleep(7); log("=== PHASE2: 6 HIT frames (post-program) ===")
for i in range(6): tx.sendto(mkframe("HIT",i),("127.0.0.1",send_port)); log("inj HIT%d"%i); time.sleep(0.6)
while time.time()<deadline: time.sleep(0.5)
log("TOTAL from guest=%d offloaded-HW-forwarded(HIT echoed back)=%d"%(cap["n"],cap["fwd"]))
if pcap: pcap.close()
