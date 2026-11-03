"""System tests: the real daemon, on its real serial, CAN-over-UDP, PPS and PTP links, against the emulators."""

import hashlib
import json
import subprocess
from pathlib import Path

import pytest

from iqk.bench import Bench, Fault

pytestmark = pytest.mark.system

ROOT = Path(__file__).resolve().parents[2]
PLAN = (ROOT / "examples" / "plans" / "bench.plan").read_text()


def plan_crc(build) -> int:
    out = subprocess.run(
        [str(build / "iqkctl"), "validate", str(ROOT / "examples/plans/bench.plan")], capture_output=True, text=True
    )
    return int(json.loads(out.stdout.strip().splitlines()[-1])["crc"], 16)


def p99(xs):
    s = sorted(abs(x) for x in xs)
    return s[int(0.99 * (len(s) - 1))]


@pytest.fixture(scope="module")
def nominal(build, tmp_path_factory):
    return Bench(tmp_path_factory.mktemp("nominal"), PLAN, seed=11).run(20)


@pytest.mark.verifies("SRS-054", "SRS-070")
def test_the_plan_is_loaded_and_telemetry_is_intact(nominal, build):
    st = nominal.status
    assert st["armed"] is True
    assert st["loader"] == 2  # done
    assert nominal.crc_errors == 0
    assert len(nominal.packets) > 1000
    assert {p.plan_crc for p in nominal.packets} == {plan_crc(build)}
    seqs = [p.seq for p in nominal.packets]
    assert seqs == list(range(seqs[0], seqs[0] + len(seqs)))


@pytest.mark.verifies("SRS-067", "SRS-060", "SRS-064")
def test_time_stamps_are_within_a_millisecond_of_true_time(nominal):
    assert nominal.status["time_source"] == 2  # PTP
    assert nominal.status["time_state"] == 2  # locked
    errs = nominal.timecode_errors_us
    assert len(errs) > 5000
    assert p99(errs) < 1000, f"p99 {p99(errs):.0f} us"


@pytest.mark.verifies("SRS-071", "SRS-073")
def test_every_sample_arrives_in_order_and_imu_channels_are_decimated(nominal):
    counts = {}
    for p in nominal.packets:
        for s in p.samples:
            counts[s["channel"]] = counts.get(s["channel"], 0) + 1
        dts = [s["dt_us"] for s in p.samples]
        assert dts == sorted(dts)
    # Module channels: one sample per data frame the module sent (the emulator's own rate is what it is). The
    # emulator keeps sending for a moment after the unit stops, so the unit may have up to 100 frames fewer,
    # but it must not have lost any while it ran: its frame counters show no gap.
    # channel 0 is aam:1:0 (group 0 of module 1), channel 4 is aam:2:0 (group 0 of module 2)
    assert nominal.status["can_errors"] == 0
    for ch, key in {0: (1, 0), 4: (2, 0)}.items():
        assert nominal.aam_sent[key] - 100 <= counts[ch] <= nominal.aam_sent[key]
    # IMU channels: gyro_z at 400 Hz, accel_z at 200 Hz, temp at 50 Hz, from the same stream.
    assert counts[5] / counts[6] == pytest.approx(2.0, rel=0.01)
    assert counts[5] / counts[7] == pytest.approx(8.0, rel=0.01)


@pytest.mark.verifies("SRS-100")
def test_replay_reproduces_the_telemetry_byte_for_byte(nominal, build):
    out = subprocess.run([str(build / "iqkctl"), "replay", str(nominal.recording)], capture_output=True, text=True)
    r = json.loads(out.stdout)
    assert r["packets"] == len(nominal.packets)
    assert r["sha256"] == hashlib.sha256(nominal.recorder_raw).hexdigest()


@pytest.mark.verifies("SRS-101", "SRS-011", "SRS-021", "SRS-003", "SRS-030", "SRS-066", "SRS-061")
def test_the_fault_campaign(build, tmp_path):
    faults = [
        Fault("imu_bit_errors", 4, 8, {"ber": 2e-4}),
        Fault("imu_garbage", 4, 8, {"rate": 0.05}),
        Fault("imu_drop", 8, 10, {"rate": 0.02}),
        Fault("imu_silent", 10, 10.2),
        Fault("imu_out_of_range", 4, 12, {"rate": 0.005}),
        Fault("aam_silent", 6, 12, {"node": 2}),
        Fault("ptp_jitter", 2, 20, {"rate": 0.1, "ms": 15}),
        Fault("ptp_outage", 12, 17),
    ]
    r = Bench(tmp_path, PLAN, faults, seed=5).run(24)
    codes = {int(k): v for k, v in r.status["fault_codes"].items()}
    assert r.crc_errors == 0
    assert codes.get(2, 0) > 0  # imu CRC errors detected
    assert codes.get(1, 0) > 0  # imu gap (the 200 ms silence) logged
    assert codes.get(3, 0) > 0  # out-of-range samples rejected
    assert codes.get(10, 0) == 1  # module 2 lost, once
    assert codes.get(26, 0) > 0  # PTP outliers discarded
    assert codes.get(24, 0) >= 2  # failed over to GNSS and back
    assert r.status["imu_crc_errors"] > 0
    # Telemetry never stopped for longer than 200 ms (module 1 and the IMU kept streaming).
    times = [p.rx_true_ns for p in r.packets]
    gaps = [b - a for a, b in zip(times, times[1:], strict=False)]
    assert max(gaps) < 200_000_000
    # Time stamps stayed good through the PTP outage (GNSS) and the jitter (outlier filter).
    assert p99(r.timecode_errors_us) < 1000


@pytest.mark.verifies("SRS-053", "SRS-054")
def test_a_module_reporting_the_wrong_configuration_crc_is_never_armed(build, tmp_path):
    r = Bench(tmp_path, PLAN, [Fault("aam_crc_wrong")], seed=3).run(6)
    assert r.status["armed"] is False
    assert r.status["loader_failure"] == 4  # crc_mismatch
    assert r.packets == []
    assert int(r.status["fault_codes"].get("31", 0)) == 1


@pytest.mark.verifies("SRS-050")
def test_a_silent_module_fails_the_load_after_retries(build, tmp_path):
    r = Bench(tmp_path, PLAN, [Fault("aam_no_response")], seed=3).run(4)
    assert r.status["armed"] is False
    assert r.status["loader_failure"] == 1  # timeout
    assert r.aam_requests == 4  # the first try and three retries


@pytest.mark.verifies("SRS-062", "SRS-066", "SRS-067")
def test_gnss_alone_keeps_time(build, tmp_path):
    r = Bench(tmp_path, PLAN, [Fault("ptp_outage")], seed=9).run(20)
    assert r.status["time_source"] == 1  # GNSS
    assert r.status["time_state"] == 2
    assert {p.time_quality for p in r.packets[-200:]} == {2}
    assert p99(r.timecode_errors_us) < 1000
