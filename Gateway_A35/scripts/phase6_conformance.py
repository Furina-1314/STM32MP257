#!/usr/bin/env python
"""Windows-behavior conformance test against the real rov_gateway board.

Implements the Windows<->A35 wire protocol (magic/version/LE fields/
CRC16-CCITT-FALSE) exactly as Salacia_Terminal does and replays the
terminal's behavior sequences over LAN: connect push, reconnect auto
queries, mode switches with Safe linkage, thruster translation modes at
zero values only, servo get/set(90), value-domain errors, heartbeat,
bad-frame resync, second-client takeover. No move(), no non-zero thruster
values, no emergency ascent - non-dangerous scope only.

Usage: python phase6_conformance.py [host] [port]
"""
import socket
import struct
import sys
import time

MAGIC = 0x53414C41
VERSION = 1
FLAG_NEED_ACK = 0x01
FLAG_EVENT = 0x02

F = {
    "ask": 0x0001, "ver": 0x0002, "status": 0x0003, "help": 0x0004,
    "stop_all": 0x0010, "emergency": 0x0011, "estop": 0x0012,
    "move_all": 0x0013, "stop_v": 0x0014, "move_v": 0x0015,
    "stop_h": 0x0016, "move_h": 0x0017,
    "safe_on": 0x0020, "safe_off": 0x0021,
    "h_on": 0x0022, "h_off": 0x0023,
    "vsync_on": 0x0024, "vsync_off": 0x0025,
    "hsync_on": 0x0026, "hsync_off": 0x0027,
    "servo_set": 0x0030, "servo_set_all": 0x0031, "servo_mid": 0x0032,
    "servo_get": 0x0033, "servo_get_all": 0x0034,
    "prop_set": 0x0040, "prop_set_all": 0x0041, "prop_stop": 0x0042,
    "prop_get_base": 0x0043, "prop_get_real": 0x0044,
    "base_value": 0x0050, "base_vh": 0x0051,
    "sensor_mpu": 0x0060, "sensor_dyp": 0x0061, "sensor_all": 0x0062,
    "heartbeat": 0x00F0,
    "summary": 0x0100, "ack": 0x0101, "state_event": 0x0102,
    "alarm": 0x0103, "state_v2": 0x0104,
}

ST_V2 = {  # bit -> name
    0: "safe", 1: "stab", 2: "gstop", 3: "vstop", 4: "hstop",
    5: "vsync", 6: "hsync", 7: "estop", 8: "emergency",
}


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def frame(func_id: int, seq: int, flags: int, payload: bytes) -> bytes:
    head = struct.pack("<IBHHBH", MAGIC, VERSION, func_id, seq, flags,
                       len(payload))
    return head + payload + struct.pack("<H", crc16(head + payload))


class Client:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.seq = 0

    def close(self):
        self.sock.close()

    def send(self, func_id, payload=b"", need_ack=True):
        flags = FLAG_NEED_ACK if need_ack else 0
        self.seq = (self.seq + 1) & 0xFFFF
        self.sock.sendall(frame(func_id, self.seq, flags, payload))
        return self.seq

    def read_frame(self, timeout=3.0):
        """Returns (func_id, seq, flags, payload) or None on timeout."""
        deadline = time.time() + timeout
        while True:
            if len(self.buf) >= 12:
                magic, ver, fid, seq, flags, ln = struct.unpack(
                    "<IBHHBH", self.buf[:12])
                if magic != MAGIC or ver != VERSION:
                    self.buf = self.buf[1:]
                    continue
                if len(self.buf) >= 14 + ln:
                    body = self.buf[:12 + ln]
                    (crc,) = struct.unpack("<H", self.buf[12 + ln:14 + ln])
                    if crc != crc16(body):
                        self.buf = self.buf[1:]
                        continue
                    self.buf = self.buf[14 + ln:]
                    return fid, seq, flags, body[12:]
            remain = deadline - time.time()
            if remain <= 0:
                return None
            self.sock.settimeout(min(remain, 0.5))
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise ConnectionError("closed by gateway")
            self.buf += chunk

    def ack_of(self, seq, timeout=4.0):
        """Reads frames until the ACK for seq arrives; returns errCode."""
        deadline = time.time() + timeout
        while True:
            remain = deadline - time.time()
            if remain <= 0:
                return None
            f = self.read_frame(min(remain, 1.0))
            if f is None:
                continue
            fid, fseq, flags, payload = f
            if fid == F["ack"] and fseq == seq:
                return struct.unpack("<H", payload)[0]
            # other frames (state events/summaries) are consumed silently

    def wait_state(self, timeout=4.0):
        """Reads until a StateEventV2; returns the mask dict."""
        deadline = time.time() + timeout
        while True:
            remain = deadline - time.time()
            if remain <= 0:
                return None
            f = self.read_frame(min(remain, 1.0))
            if f is None:
                continue
            fid, _, _, payload = f
            if fid == F["state_v2"] and len(payload) == 3:
                mask = struct.unpack("<H", payload[1:3])[0]
                return {name: bool(mask >> bit & 1)
                        for bit, name in ST_V2.items()}


failures = []


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}: {name}" + (f" [{detail}]" if detail and not ok else ""))
    if not ok:
        failures.append(name)


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.120"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 7000
    c = Client(host, port)
    print(f"connected to {host}:{port}")

    # 1) connect-time authoritative state push
    st = c.wait_state(2.0)
    check("connect-time StateEventV2", st is not None)
    if st:
        print(f"  state: {st}")
        check("alignment stab/vsync/hsync on", st["stab"] and st["vsync"] and st["hsync"])

    # 2) Windows reconnect auto-queries: ask / status / sensor all
    for name in ("ask", "status", "sensor_all"):
        s = c.send(F[name])
        check(f"{name} ACK ok", c.ack_of(s) == 0)

    # 3) summary stream ~100 Hz
    n, t0 = 0, time.time()
    while time.time() - t0 < 2.0:
        f = c.read_frame(0.5)
        if f and f[0] == F["summary"]:
            assert len(f[3]) == 45
            n += 1
    print(f"  summaries: {n} in 2s (~{n // 2} Hz)")
    check("summary stream >= 40/2s", n >= 40)

    # 4) mode switches with Safe linkage
    s = c.send(F["safe_on"]); e = c.ack_of(s)
    check("safe on ACK ok", e == 0, f"err={e}")
    st = c.wait_state()
    check("state safe=1 stab=1", st and st["safe"] and st["stab"])
    s = c.send(F["h_off"])
    check("horizontal off under safe -> errCode 7", c.ack_of(s) == 7)
    s = c.send(F["safe_off"]); e = c.ack_of(s)
    check("safe off ACK ok", e == 0, f"err={e}")
    s = c.send(F["h_off"]); e = c.ack_of(s)
    check("horizontal off after safe off -> ok", e == 0, f"err={e}")
    st = c.wait_state()
    check("state stab=0", st and not st["stab"])

    # 5) thruster zero-value / Stop-Move / translation verification
    #    (bench scope per VibePrompt step 7: all targets are zero, move()
    #    only clears the latch and cannot produce motion)
    s = c.send(F["move_all"]); e = c.ack_of(s)
    check("move all (zero targets) -> ok", e == 0, f"err={e}")
    st = c.wait_state()
    check("state gstop=0 after move", st and not st["gstop"])
    s = c.send(F["prop_set"], struct.pack("<Bh", 10, 0))
    check("prop CH10 individual (stab off) -> ok", c.ack_of(s) == 0)
    s = c.send(F["h_on"]); e = c.ack_of(s)
    check("horizontal on -> ok", e == 0, f"err={e}")
    s = c.send(F["prop_set"], struct.pack("<Bh", 11, 0))
    check("prop CH11 base path (stab on) -> ok", c.ack_of(s) == 0)
    s = c.send(F["base_vh"], struct.pack("<hh", 0, 0))
    check("base value vh 0,0 -> ok", c.ack_of(s) == 0)
    s = c.send(F["hsync_off"]); e = c.ack_of(s)
    check("horizontal sync off -> ok", e == 0, f"err={e}")
    s = c.send(F["prop_set"], struct.pack("<Bh", 14, 0))
    check("prop CH14 individual (sync off) -> ok", c.ack_of(s) == 0)
    s = c.send(F["hsync_on"]); e = c.ack_of(s)
    check("horizontal sync on -> ok", e == 0, f"err={e}")
    # thruster sets rejected while globally stopped (verified next round)
    s = c.send(F["stop_all"])
    check("stop all -> ok", c.ack_of(s) == 0)
    s = c.send(F["prop_set"], struct.pack("<Bh", 10, 0))
    check("prop set under global stop -> errCode 7", c.ack_of(s) == 7)
    s = c.send(F["move_all"])
    check("move all re-enable -> ok", c.ack_of(s) == 0)

    # 6) group stop/move latches (no move_all: stays within stop family)
    s = c.send(F["stop_v"])
    check("stop vertical -> ok", c.ack_of(s) == 0)
    st = c.wait_state()
    check("state vstop=1", st and st["vstop"])
    s = c.send(F["move_v"])
    check("move vertical -> ok", c.ack_of(s) == 0)
    st = c.wait_state()
    check("state vstop=0", st and not st["vstop"])

    # 7) servo set/get/mid at neutral angle
    s = c.send(F["servo_set"], struct.pack("<BH", 3, 90))
    check("servo set 3@90 -> ok", c.ack_of(s) == 0)
    s = c.send(F["servo_get"], struct.pack("<B", 3))
    check("servo get 3 ACK ok", c.ack_of(s) == 0)
    got_data = None
    for _ in range(5):
        f = c.read_frame(1.0)
        if f and f[0] == F["servo_get"]:
            got_data = struct.unpack("<h", f[3][:2])[0]
            break
    check("servo get data frame = 90", got_data == 90)
    s = c.send(F["servo_mid"], struct.pack("<B", 0xFF))
    check("servo mid broadcast -> ok", c.ack_of(s) == 0)

    # 8) independent validation of value domains (Windows pre-checks; A35
    #    must reject independently)
    s = c.send(F["servo_set"], struct.pack("<BH", 3, 181))
    check("servo angle 181 -> errCode 2", c.ack_of(s) == 2)
    s = c.send(F["prop_set"], struct.pack("<Bh", 10, 101))
    check("prop value 101 -> errCode 2", c.ack_of(s) == 2)
    s = c.send(F["base_vh"], struct.pack("<hh", -101, 0))
    check("base vh -101 -> errCode 2", c.ack_of(s) == 2)

    # 9) deprecated / unknown
    s = c.send(F["base_value"])
    check("deprecated 0x0050 -> errCode 6", c.ack_of(s) == 6)
    s = c.send(0x0099)
    check("unknown funcId -> errCode 6", c.ack_of(s) == 6)

    # 10) heartbeat: silent (no ACK)
    before = len(c.buf)
    c.send(F["heartbeat"], struct.pack("<I", 123456), need_ack=False)
    time.sleep(0.5)
    f = c.read_frame(0.5)
    hb_acked = f is not None and f[0] == F["ack"]
    check("heartbeat silent (no ACK)", not hb_acked)

    # 11) framing robustness: garbage then a valid frame
    c.sock.sendall(b"\x00\x11\x22")
    s = c.send(F["ask"])
    check("garbage + valid frame -> resync + ACK", c.ack_of(s) == 0)

    # 12) estop path and final latched state
    t0 = time.time()
    s = c.send(F["estop"])
    e = c.ack_of(s)
    rtt = (time.time() - t0) * 1000
    check("estop ACK ok", e == 0, f"err={e}")
    print(f"  estop RTT over LAN: {rtt:.1f} ms")
    check("estop RTT < 100 ms", rtt < 100)
    st = c.wait_state()
    check("final state gstop+estop", st and st["gstop"] and st["estop"])

    # 13) second client takeover (default policy)
    c2 = Client(host, port)
    st2 = c2.wait_state(2.0)
    check("second client connect push", st2 is not None)
    time.sleep(0.3)
    old_closed = False
    try:
        c.sock.settimeout(1.0)
        while True:
            chunk = c.sock.recv(256)
            if not chunk:
                old_closed = True
                break
    except socket.timeout:
        pass
    except ConnectionError:
        old_closed = True
    check("takeover closes old client", old_closed)
    c.close()

    # 14) new owner can still work
    s = c2.send(F["ask"])
    check("new owner ask ACK ok", c2.ack_of(s) == 0)
    c2.close()

    print("-" * 40)
    print(f"{'ALL PASS' if not failures else 'FAILURES: ' + ', '.join(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
