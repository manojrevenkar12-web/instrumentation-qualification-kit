"""reports/console.json: everything the evidence console shows, assembled from generated reports only."""

from __future__ import annotations

import json
from datetime import UTC, datetime

import yaml

from .icd import ROOT
from .icd import keys as icd_keys
from .icd import load as load_icd

R = ROOT / "reports"
FAULT_NAMES = {
    1: "IMU gap",
    2: "IMU CRC error",
    3: "IMU out of range",
    4: "IMU lost",
    10: "Module lost",
    11: "Module frame gap",
    12: "Module fault",
    20: "Clock stepped",
    21: "Lost lock",
    22: "Holdover",
    23: "Free-running",
    24: "Time source switched",
    25: "GNSS time rejected",
    26: "PTP outlier",
    30: "Plan rejected",
    31: "Plan load failed",
    40: "Bad diagnostic request",
}
AREAS = [
    ("Interfaces", 1, 9),
    ("Serial links", 10, 19),
    ("IMU", 20, 29),
    ("Acquisition modules", 30, 39),
    ("Measurement plan", 40, 49),
    ("Plan loading", 50, 59),
    ("Time", 60, 69),
    ("Telemetry", 70, 79),
    ("Diagnostics", 80, 89),
    ("Software update", 90, 99),
    ("Determinism and assurance", 100, 119),
]


def _load(p, default=None):
    return json.loads(p.read_text()) if p.exists() else default


def bench_summary(result, name: str, faults: list) -> dict:
    errs = sorted(abs(x) for x in result.timecode_errors_us)

    def pct(q):
        return round(errs[int(q * (len(errs) - 1))], 1) if errs else None

    bins = [0, 50, 100, 200, 300, 500, 750, 1000, 2000, 5000]
    hist = [sum(1 for e in errs if lo <= e < hi) for lo, hi in zip(bins, bins[1:] + [float("inf")], strict=False)]
    q = {}
    for p in result.packets:
        q[p.time_quality] = q.get(p.time_quality, 0) + 1
    codes = {FAULT_NAMES.get(int(k), k): v for k, v in result.status.get("fault_codes", {}).items()}
    return {
        "name": name,
        "seconds": round(result.elapsed_s, 1),
        "faults_injected": [
            {"name": f.name, "from_s": f.at, "to_s": f.until if f.until < 1e8 else None} for f in faults
        ],
        "packets": len(result.packets),
        "samples": result.status.get("samples"),
        "crc_errors_at_receiver": result.crc_errors,
        "time_source": result.status.get("time_source"),
        "time_state": result.status.get("time_state"),
        "time_error_us": {
            "n": len(errs),
            "p50": pct(0.5),
            "p95": pct(0.95),
            "p99": pct(0.99),
            "max": round(errs[-1], 1) if errs else None,
        },
        "time_error_histogram": {"edges_us": bins, "counts": hist},
        "time_quality": {str(k): v for k, v in sorted(q.items())},
        "faults_logged": codes,
        "imu_crc_errors": result.status.get("imu_crc_errors"),
    }


def build() -> dict:
    trace = _load(R / "qualification" / "traceability.json", {})
    reqs = trace.get("requirements", [])
    areas = []
    for name, lo, hi in AREAS:
        ids = [r for r in reqs if lo <= int(r["id"].split("-")[1]) <= hi]
        areas.append({"name": name, "requirements": [r["id"] for r in ids]})
    heldout = {}
    for p in sorted((R / "heldout").glob("*/first_run.json")):
        d = _load(p)
        status = (ROOT / "heldout" / p.parent.name / "STATUS").read_text().strip()
        heldout[p.parent.name] = {
            "summary": d["summary"],
            "status": status,
            "failed": [
                {"name": r["name"], "kind": r["kind"], "detail": r["detail"]} for r in d["results"] if not r["pass"]
            ],
        }
    assurance = {
        n: _load(R / "assurance" / f"{n}.json") for n in ("repro", "hardening", "static", "sanitize", "sbom", "release")
    }
    sbom = _load(R / "assurance" / "sbom.cdx.json", {})
    cfg = yaml.safe_load((ROOT / "config" / "unit.yaml").read_text())
    return {
        "generated_at": datetime.now(UTC).isoformat(timespec="seconds"),
        "unit": {
            "sw_version": ".".join(str(v) for v in cfg["unit"]["sw_version"]),
            "hw_id": f"0x{cfg['unit']['hw_id']:08X}",
        },
        "icds": [
            {
                "key": k,
                "id": load_icd(k).icd,
                "version": load_icd(k).version,
                "title": load_icd(k).title,
                "messages": len(load_icd(k).messages),
            }
            for k in icd_keys()
        ],
        "traceability": {"summary": trace.get("summary"), "requirements": reqs, "areas": areas},
        "bench": {n: _load(R / "bench" / f"{n}.json") for n in ("nominal", "campaign")},
        "heldout": heldout,
        "agents": _load(R / "agents" / "experiment.json"),
        "assurance": {
            k: ({kk: vv for kk, vv in v.items() if kk != "details"} if v else None) for k, v in assurance.items()
        },
        "sbom": [
            {"name": c["name"], "version": c.get("version"), "scope": c.get("scope", "required")}
            for c in sbom.get("components", [])
        ],
        "release": {
            "manifest": _load(R / "release" / "manifest.json"),
            "build_signature": _load(R / "release" / "build.sig.json"),
        },
    }


def write() -> dict:
    data = build()
    (R / "console.json").write_text(json.dumps(data, indent=1) + "\n")
    return data
