"""Software-in-the-loop bench: emulators of every device the unit integrates, and the real daemon between them.

Each emulator speaks its device's ICD on the same kind of link the unit uses: the IMU and the GNSS receiver on
pseudo-terminals (raw serial), the acquisition modules as CAN frames over UDP, the PPS line as one datagram per
edge, PTP as real IEEE 1588 messages over UDP. All of them run on one "true time" (the bench's UTC), so the
bench can measure how far the unit's time stamps are from the truth. Faults are injected by name and time.

The same scenarios run against hardware by pointing the bench at real ports (targets/hardware.yaml); this
repository has only the emulators.
"""

from __future__ import annotations

import math
import os
import random
import socket
import struct
import subprocess
import threading
import time
import tty
import zlib
from dataclasses import dataclass, field
from pathlib import Path

from . import icd as I

ROOT = I.ROOT
BUILD = Path(os.environ.get("IQK_BUILD", ROOT / "build"))

IMU = I.load("imu")
GNSS = I.load("gnss")
AAM = I.load("aam")
PTP = I.load("ptp")
TLM = I.load("tlm")

GPS_EPOCH_UNIX = 315_964_800
LEAP_S = 18


def crc16(data: bytes) -> int:
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def frame(msg_id: int, payload: bytes) -> bytes:
    body = bytes([msg_id, len(payload)]) + payload
    return b"\xa5\x5a" + body + crc16(body).to_bytes(2, "big")


def free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class TrueTime:
    """The bench's UTC: one reference for every emulator and for scoring the unit."""

    def __init__(self) -> None:
        self.offset = time.time_ns() - time.monotonic_ns()

    def now(self) -> int:
        return time.monotonic_ns() + self.offset

    def utc_of_mono(self, mono: int) -> int:
        return mono + self.offset


@dataclass
class Fault:
    """A fault, active from `at` to `until` seconds after the bench starts."""

    name: str
    at: float = 0.0
    until: float = 1e9
    params: dict = field(default_factory=dict)

    def active(self, t: float) -> bool:
        return self.at <= t < self.until


class Emulator(threading.Thread):
    def __init__(self, bench: Bench) -> None:
        super().__init__(daemon=True)
        self.bench = bench
        self.stop_flag = threading.Event()
        self.rng = random.Random(bench.seed + hash(type(self).__name__) % 1000)

    def fault(self, name: str) -> Fault | None:
        t = self.bench.elapsed()
        for f in self.bench.faults:
            if f.name == name and f.active(t):
                return f
        return None

    def stop(self) -> None:
        self.stop_flag.set()


class ImuEmulator(Emulator):
    """IMU-SER: 400 Hz samples on a pseudo-terminal; answers self-test, start and stop commands."""

    def __init__(self, bench: Bench) -> None:
        super().__init__(bench)
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        tty.setraw(self.master)
        self.path = os.ttyname(self.slave)
        os.set_blocking(self.master, False)
        self.streaming = False
        self.seq = 0
        self.drift_ppm = bench.rng.uniform(-40, 40)
        self.sent = 0
        self.samples: list[tuple[int, int]] = []  # (seq, true acquisition time)

    def run(self) -> None:
        period = 1 / IMU.message("imu_sample").rate_hz
        t0 = time.monotonic()
        k = 0
        msg = IMU.message("imu_sample")
        while not self.stop_flag.is_set():
            self._commands()
            k += 1
            target = t0 + k * period
            delay = target - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            if not self.streaming or self.fault("imu_silent"):
                continue
            true_t = self.bench.truth.now()
            dev_us = int((time.monotonic_ns() * (1 + self.drift_ppm * 1e-6)) // 1000) & 0xFFFFFFFF
            phase = 2 * math.pi * 2.0 * (true_t / 1e9)
            v = {
                "seq": self.seq,
                "t_us": dev_us,
                "gyro_x": int(0.1 * math.sin(phase) / 1e-6),
                "gyro_y": 0,
                "gyro_z": int(0.5 * math.cos(phase) / 1e-6),
                "accel_x": 0,
                "accel_y": 0,
                "accel_z": int(-9.81 / 1e-5),
                "temp": int(31.5 / 0.01),
                "status": 0b0111,
            }
            if f := self.fault("imu_out_of_range"):
                if self.rng.random() < f.params.get("rate", 0.01):
                    v["gyro_x"] = int(20.0 / 1e-6)
            skip = self.fault("imu_drop")
            self.samples.append((self.seq, true_t))
            self.seq = (self.seq + 1) & 0xFFFF
            if skip and self.rng.random() < skip.params.get("rate", 0.01):
                continue
            data = frame(msg.id, I.encode(IMU, msg, v))
            if f := self.fault("imu_bit_errors"):
                b = bytearray(data)
                for i in range(len(b)):
                    if self.rng.random() < f.params.get("ber", 1e-4) * 8:
                        b[i] ^= 1 << self.rng.randrange(8)
                data = bytes(b)
            if f := self.fault("imu_garbage"):
                if self.rng.random() < f.params.get("rate", 0.05):
                    data = bytes([0xA5, 0x5A]) + bytes(self.rng.randrange(256) for _ in range(5)) + data
            self._write(data)
            self.sent += 1

    def _write(self, data: bytes) -> None:
        try:
            os.write(self.master, data)
        except BlockingIOError:
            pass

    def _commands(self) -> None:
        try:
            data = os.read(self.master, 4096)
        except (BlockingIOError, OSError):
            return
        i = 0
        while i + 4 <= len(data):
            if data[i] != 0xA5 or data[i + 1] != 0x5A:
                i += 1
                continue
            n = data[i + 3]
            body = data[i + 2 : i + 4 + n]
            if crc16(body) != int.from_bytes(data[i + 4 + n : i + 6 + n], "big"):
                i += 1
                continue
            cmd = data[i + 4]
            if cmd == 1:
                self.streaming = True
            elif cmd == 2:
                self.streaming = False
            elif cmd == 3:
                m = IMU.message("imu_bit_result")
                fail = self.fault("imu_bit_fail")
                self._write(frame(m.id, I.encode(IMU, m, {"result": 1 if fail else 0, "code": 7 if fail else 0})))
            i += 6 + n


class GnssEmulator(Emulator):
    """GNSS-SER: a PPS datagram at each true UTC second, then the time_pulse message naming it."""

    def __init__(self, bench: Bench) -> None:
        super().__init__(bench)
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)
        tty.setraw(self.master)
        self.path = os.ttyname(self.slave)
        self.pps = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def run(self) -> None:
        msg = GNSS.message("time_pulse")
        while not self.stop_flag.is_set():
            now = self.bench.truth.now()
            next_s = (now // 1_000_000_000 + 1) * 1_000_000_000
            time.sleep(max(0.0, (next_s - now) / 1e9))
            if self.stop_flag.is_set():
                return
            if self.fault("gnss_outage"):
                continue
            self.pps.sendto(b"\x01", ("127.0.0.1", self.bench.ports["pps"]))
            time.sleep(0.15 + self.rng.uniform(0, 0.1))
            gps_s = next_s // 1_000_000_000 - GPS_EPOCH_UNIX + LEAP_S
            if f := self.fault("gnss_wrong_second"):
                gps_s += int(f.params.get("seconds", 1))
            flags = 0b111
            if self.fault("gnss_time_invalid"):
                flags = 0b110
            acc = 30
            if f := self.fault("gnss_poor_accuracy"):
                acc = int(f.params.get("acc_ns", 5000))
            v = {
                "week": gps_s // 604800,
                "tow_ms": (gps_s % 604800) * 1000,
                "leap_s": LEAP_S,
                "fix": 3,
                "flags": flags,
                "acc_ns": acc,
            }
            if self.fault("gnss_late"):
                time.sleep(1.0)
            os.write(self.master, frame(msg.id, I.encode(GNSS, msg, v)))


class AamEmulator(Emulator):
    """AAM-CAN: modules with registers, commit and CRC, heartbeats, and multiplexed data at the configured rates.

    Input 0 of module 1 carries a time code (true UTC milliseconds within the second, 10 mV per ms), so the
    bench can score the unit's time stamps end to end."""

    def __init__(self, bench: Bench, nodes: tuple[int, ...] = (1, 2)) -> None:
        super().__init__(bench)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", bench.ports["can_peer"]))
        self.sock.settimeout(0.0005)
        self.nodes = nodes
        n = AAM.extra["channels"]
        self.regs = {node: [{"enable": 0, "range": 0, "divisor": 1, "filter": 0} for _ in range(n)] for node in nodes}
        self.applied = {node: [dict(r) for r in self.regs[node]] for node in nodes}
        self.state = {node: 1 for node in nodes}  # pre-operational
        self.counters = {node: [0, 0, 0, 0] for node in nodes}
        self.hb_counter = {node: 0 for node in nodes}
        self.requests = 0
        self.timecode: list[tuple[int, int]] = []  # (true time of sample, raw)

    def send(self, can_id: int, payload: bytes) -> None:
        pkt = struct.pack("<IB", can_id, len(payload)) + payload.ljust(8, b"\x00")
        self.sock.sendto(pkt, ("127.0.0.1", self.bench.ports["can"]))

    def config_crc(self, node: int) -> int:
        b = b"".join(struct.pack("<IIII", r["enable"], r["range"], r["divisor"], r["filter"]) for r in self.applied[node])
        return zlib.crc32(b)

    def handle(self, pkt: bytes) -> None:
        can_id, dlc = struct.unpack_from("<IB", pkt)
        data = pkt[5 : 5 + dlc]
        if can_id == 0x000 and dlc == 2:
            cmd, node = data
            if node in self.state and cmd == 0x01:
                self.state[node] = 2
            return
        node = can_id - 0x600
        if node not in self.regs:
            return
        self.requests += 1
        if self.fault("aam_no_response"):
            return
        req = I.decode(AAM, AAM.message("aam_reg_request"), data)
        if req is None:
            return
        cmd, index, sub, value = req["cmd"], req["index"], req["sub"], req["value"]
        resp = {"cmd": 0x80, "index": index, "sub": sub, "value": 0x06020000}
        if f := self.fault("aam_abort"):
            if self.rng.random() < f.params.get("rate", 1.0):
                self._resp(node, resp)
                return
        names = {1: "enable", 2: "range", 3: "divisor", 4: "filter"}
        if 0x2000 <= index < 0x2000 + len(self.regs[node]) and sub in names:
            ch = self.regs[node][index - 0x2000]
            if cmd == 0x23:
                ch[names[sub]] = value
                resp = {"cmd": 0x60, "index": index, "sub": sub, "value": 0}
            elif cmd == 0x40:
                v = ch[names[sub]]
                if self.fault("aam_readback_corrupt") and sub == 2:
                    v ^= 1
                resp = {"cmd": 0x43, "index": index, "sub": sub, "value": v}
        elif index == 0x1F00 and cmd == 0x23:
            self.applied[node] = [dict(r) for r in self.regs[node]]
            resp = {"cmd": 0x60, "index": index, "sub": sub, "value": 0}
        elif index == 0x1F01 and cmd == 0x40:
            crc = self.config_crc(node)
            if self.fault("aam_crc_wrong"):
                crc ^= 0xFFFF
            resp = {"cmd": 0x43, "index": index, "sub": sub, "value": crc}
        self._resp(node, resp)

    def _resp(self, node: int, resp: dict) -> None:
        if self.fault("aam_slow_response"):
            time.sleep(0.08)
        self.send(0x580 + node, I.encode(AAM, AAM.message("aam_reg_response"), resp))

    def raw(self, node: int, ch: int, true_t: int) -> int:
        cfg = self.applied[node][ch]
        full_mv = AAM.extra["ranges_mv"][min(cfg["range"], 3)]
        if node == 1 and ch == 0:
            mv = (true_t % 1_000_000_000) / 1e6 * 10.0  # time code: 10 mV per millisecond within the second
        else:
            mv = 0.3 * full_mv * math.sin(2 * math.pi * (1 + ch) * true_t / 1e9)
        if f := self.fault("aam_overrange"):
            if f.params.get("node", node) == node:
                mv = full_mv * 1.5
        return max(-32767, min(32767, round(mv / full_mv * 32767)))

    def run(self) -> None:
        base = AAM.extra["base_rate_hz"]
        tick = 0
        t0 = time.monotonic()
        next_hb = t0
        while not self.stop_flag.is_set():
            try:
                while True:
                    pkt, _ = self.sock.recvfrom(64)
                    if len(pkt) == 13:
                        self.handle(pkt)
            except (TimeoutError, BlockingIOError, OSError):
                pass
            now = time.monotonic()
            if now >= next_hb:
                next_hb += 1.0
                for node in self.nodes:
                    if self.fault("aam_silent") and self.fault("aam_silent").params.get("node", node) == node:
                        continue
                    errors = 0b10 if self.fault("aam_overrange") else 0
                    hb = {"state": self.state[node], "counter": self.hb_counter[node] & 0xFF, "errors": errors}
                    self.hb_counter[node] += 1
                    self.send(0x700 + node, I.encode(AAM, AAM.message("aam_heartbeat"), hb))
            # 1 kHz base tick
            target = t0 + (tick + 1) / base
            if now < target:
                continue
            tick += 1
            true_t = self.bench.truth.now()
            for node in self.nodes:
                if self.state[node] != 2:
                    continue
                silent = self.fault("aam_silent")
                if silent and silent.params.get("node", node) == node:
                    continue
                for g in range(AAM.extra["channel_groups"]):
                    chans = self.applied[node][g * 3 : g * 3 + 3]
                    enabled = [c for c in chans if c["enable"]]
                    if not enabled:
                        continue
                    div = min(c["divisor"] for c in enabled)
                    if tick % max(div, 1):
                        continue
                    raws = [self.raw(node, g * 3 + i, true_t) if chans[i]["enable"] else 0 for i in range(3)]
                    if node == 1 and g == 0 and chans[0]["enable"]:
                        self.timecode.append((true_t, raws[0]))
                    self.counters[node][g] = (self.counters[node][g] + 1) & 0xFF
                    if (f := self.fault("aam_drop")) and self.rng.random() < f.params.get("rate", 0.01):
                        continue
                    d = {"mux": g, "counter": self.counters[node][g], "ch_a": raws[0], "ch_b": raws[1], "ch_c": raws[2]}
                    self.send(0x180 + node, I.encode(AAM, AAM.message("aam_data"), d))


class PtpMaster(Emulator):
    """PTP grandmaster: two-step Sync and Follow_Up at 8 Hz, Delay_Resp per Delay_Req, all in true time."""

    CLOCK_ID = 0x00163EFFFE4D5A01

    def __init__(self, bench: Bench) -> None:
        super().__init__(bench)
        self.ev = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.ev.bind(("127.0.0.1", bench.ports["ptp_master_event"]))
        self.ev.settimeout(0.005)
        self.gen = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.seq = 0
        self.exchanges = 0

    def header(self, msg_type: int, length: int, seq: int, control: int) -> dict:
        return {
            "type_sdo": msg_type,
            "version": 2,
            "length": length,
            "domain": 0,
            "minor_sdo": 0,
            "flags": 1 << 9 if msg_type == 0 else 0,
            "correction": 0,
            "type_specific": 0,
            "clock_identity": self.CLOCK_ID,
            "port_number": 1,
            "sequence_id": seq,
            "control": control,
            "log_interval": -3,
        }

    def master_time(self) -> int:
        t = self.bench.truth.now()
        if f := self.fault("ptp_master_step"):
            t += int(f.params.get("ms", 5) * 1_000_000)
        return t

    def run(self) -> None:
        period = 1 / PTP.message("ptp_sync").rate_hz
        next_sync = time.monotonic()
        while not self.stop_flag.is_set():
            now = time.monotonic()
            if now >= next_sync:
                next_sync += period
                if not self.fault("ptp_outage"):
                    self.sync()
            try:
                data, _ = self.ev.recvfrom(256)
                t4 = self.master_time()
                self.delay_req(data, t4)
            except (TimeoutError, OSError):
                pass

    def _send(self, sock: socket.socket, port_key: str, data: bytes, one_way_extra_s: float = 0.0) -> None:
        if one_way_extra_s > 0:
            time.sleep(one_way_extra_s)
        sock.sendto(data, ("127.0.0.1", self.bench.ports[port_key]))

    def sync(self) -> None:
        self.seq = (self.seq + 1) & 0xFFFF
        m = PTP.message("ptp_sync")
        extra = 0.0
        if f := self.fault("ptp_asymmetry"):
            extra = f.params.get("ms", 0.5) / 1000
        if (f := self.fault("ptp_jitter")) and self.rng.random() < f.params.get("rate", 0.2):
            extra += f.params.get("ms", 20) / 1000
        t1 = self.master_time()
        self._send(self.ev, "ptp_event", I.encode(PTP, m, {**self.header(0x0, 44, self.seq, 0), "origin_s": 0, "origin_ns": 0}), extra)
        fu = PTP.message("ptp_follow_up")
        v = {**self.header(0x8, 44, self.seq, 2), "origin_s": t1 // 1_000_000_000, "origin_ns": t1 % 1_000_000_000}
        self._send(self.gen, "ptp_general", I.encode(PTP, fu, v))

    def delay_req(self, data: bytes, t4: int) -> None:
        req = I.decode(PTP, PTP.message("ptp_delay_req"), data)
        if req is None or (req["type_sdo"] & 0xF) != 1:
            return
        self.exchanges += 1
        m = PTP.message("ptp_delay_resp")
        v = {
            **self.header(0x9, 54, req["sequence_id"], 3),
            "receive_s": t4 // 1_000_000_000,
            "receive_ns": t4 % 1_000_000_000,
            "req_clock_identity": req["clock_identity"],
            "req_port_number": req["port_number"],
        }
        self._send(self.gen, "ptp_general", I.encode(PTP, m, v))


@dataclass
class Packet:
    rx_true_ns: int
    seq: int
    time_ns: int
    time_quality: int
    plan_crc: int
    samples: list[dict]


class Recorder(Emulator):
    """The telemetry receiver: checks each datagram's CRC and decodes it."""

    def __init__(self, bench: Bench) -> None:
        super().__init__(bench)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", bench.ports["tlm"]))
        self.sock.settimeout(0.05)
        self.packets: list[Packet] = []
        self.raw = bytearray()
        self.crc_errors = 0
        self.decode_errors = 0

    def run(self) -> None:
        msg = TLM.message("tlm_packet")
        while not self.stop_flag.is_set():
            try:
                data, _ = self.sock.recvfrom(2048)
            except (TimeoutError, OSError):
                continue
            rx = self.bench.truth.now()
            self.raw += data
            body, trailer = data[:-4], data[-4:]
            if zlib.crc32(body) != int.from_bytes(trailer, "little"):
                self.crc_errors += 1
                continue
            v = I.decode(TLM, msg, body)
            if v is None:
                self.decode_errors += 1
                continue
            self.packets.append(Packet(rx, v["seq"], v["time_ns"], v["time_quality"], v["plan_crc"], v["samples"]))


@dataclass
class Result:
    status: dict
    packets: list[Packet]
    recorder_raw: bytes
    recording: Path
    timecode_errors_us: list[float]
    imu_sent: int
    aam_requests: int
    ptp_exchanges: int
    crc_errors: int
    elapsed_s: float
    daemon_stderr: str


class Bench:
    def __init__(self, workdir: Path, plan: str | None, faults: list[Fault] | None = None, seed: int = 7,
                 nodes: tuple[int, ...] = (1, 2)) -> None:
        self.workdir = workdir
        workdir.mkdir(parents=True, exist_ok=True)
        self.plan = plan
        self.faults = faults or []
        self.seed = seed
        self.rng = random.Random(seed)
        self.truth = TrueTime()
        self.ports = {k: free_port() for k in ("pps", "can", "can_peer", "ptp_event", "ptp_general", "ptp_master_event", "tlm", "diag")}
        self.nodes = nodes
        self.t0 = time.monotonic()

    def elapsed(self) -> float:
        return time.monotonic() - self.t0

    def run(self, seconds: float) -> Result:
        imu, gnss = ImuEmulator(self), GnssEmulator(self)
        aam, ptp, rec = AamEmulator(self, self.nodes), PtpMaster(self), Recorder(self)
        plan_path = self.workdir / "plan.plan"
        if self.plan is not None:
            plan_path.write_text(self.plan)
        recording = self.workdir / "run.rec"
        status = self.workdir / "status.json"
        args = [
            str(BUILD / "daud"),
            "--imu-tty", imu.path, "--gnss-tty", gnss.path,
            "--pps-port", str(self.ports["pps"]),
            "--can-port", str(self.ports["can"]), "--can-peer", f"127.0.0.1:{self.ports['can_peer']}",
            "--ptp-event-port", str(self.ports["ptp_event"]), "--ptp-general-port", str(self.ports["ptp_general"]),
            "--ptp-master", f"127.0.0.1:{self.ports['ptp_master_event']}",
            "--tlm-dest", f"127.0.0.1:{self.ports['tlm']}", "--diag-port", str(self.ports["diag"]),
            "--record", str(recording), "--status", str(status), "--run-ms", str(int(seconds * 1000)),
        ]
        if self.plan is not None:
            args += ["--plan", str(plan_path)]
        for e in (imu, gnss, aam, ptp, rec):
            e.start()
        self.t0 = time.monotonic()
        proc = subprocess.run(args, capture_output=True, text=True, timeout=seconds + 30)
        for e in (imu, gnss, aam, ptp, rec):
            e.stop()
        for e in (imu, gnss, aam, ptp, rec):
            e.join(timeout=2)
        import json

        st = json.loads(status.read_text()) if status.exists() else {}
        return Result(
            status=st,
            packets=rec.packets,
            recorder_raw=bytes(rec.raw),
            recording=recording,
            timecode_errors_us=timecode_errors(rec.packets, plan_channel_index(self.plan, "aam:1:0")),
            imu_sent=imu.sent,
            aam_requests=aam.requests,
            ptp_exchanges=ptp.exchanges,
            crc_errors=rec.crc_errors,
            elapsed_s=time.monotonic() - self.t0,
            daemon_stderr=proc.stderr,
        )


def plan_channel_index(plan: str | None, source: str) -> int | None:
    if plan is None:
        return None
    idx = 0
    for line in plan.splitlines():
        t = line.split("#")[0].split()
        if t and t[0] == "channel":
            if t[2] == source:
                return idx
            idx += 1
    return None


def timecode_errors(packets: list[Packet], channel: int | None) -> list[float]:
    """Time-stamp error (µs) of each time-code sample: the stamp's milliseconds-within-second against the value
    the module encoded from true time. Only samples stamped while the unit reported a locked time source."""
    if channel is None:
        return []
    out = []
    for p in packets:
        if p.time_quality < 2:
            continue
        for s in p.samples:
            if s["channel"] != channel:
                continue
            stamp_ns = p.time_ns + s["dt_us"] * 1000
            encoded_ms = s["value"] * 1e-4 * 100.0  # value in volts (scale 1); 10 mV per ms
            stamp_ms = (stamp_ns % 1_000_000_000) / 1e6
            err_ms = (stamp_ms - encoded_ms + 500) % 1000 - 500
            out.append(err_ms * 1000)
    return out
