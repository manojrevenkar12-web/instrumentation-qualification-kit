"""Test evidence: run the unit and system tests and read which requirements each test claims to verify.

Claims come from the tests themselves (IQK_VERIFIES in C++, @pytest.mark.verifies in Python) and are read
back from the JUnit reports, so the matrix cannot list a test that did not run.
"""

from __future__ import annotations

import os
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import asdict, dataclass, field
from pathlib import Path

from ..icd import ROOT


@dataclass
class TestResult:
    id: str  # gtest:Suite.Name or pytest:path::name
    passed: bool
    verifies: list[str] = field(default_factory=list)
    seconds: float = 0.0
    message: str = ""


def build_dir() -> Path:
    return Path(os.environ.get("IQK_BUILD", ROOT / "build"))


def parse_gtest_xml(path: Path) -> list[TestResult]:
    out = []
    for case in ET.parse(path).getroot().iter("testcase"):
        if case.get("result") == "suppressed" or case.get("status") == "notrun":
            continue
        props = {p.get("name"): p.get("value") for p in case.iter("property")}
        # gtest also writes RecordProperty values as attributes of <testcase>
        claim = props.get("verifies") or case.get("verifies") or ""
        failure = case.find("failure")
        out.append(
            TestResult(
                id=f"gtest:{case.get('classname')}.{case.get('name')}",
                passed=failure is None,
                verifies=[x for x in claim.split(",") if x],
                seconds=float(case.get("time", 0) or 0),
                message=(failure.get("message", "")[:400] if failure is not None else ""),
            )
        )
    return out


def parse_pytest_xml(path: Path) -> list[TestResult]:
    out = []
    for case in ET.parse(path).getroot().iter("testcase"):
        if case.find("skipped") is not None:
            continue
        props = {p.get("name"): p.get("value") for p in case.iter("property")}
        bad = case.find("failure") if case.find("failure") is not None else case.find("error")
        cls = case.get("classname", "").replace(".", "/")
        name = case.get("name", "").split("[")[0]  # parameterised cases count as one test
        out.append(
            TestResult(
                id=f"pytest:{cls}.py::{name}",
                passed=bad is None,
                verifies=[x for x in (props.get("verifies") or "").split(",") if x],
                seconds=float(case.get("time", 0) or 0),
                message=(bad.get("message", "")[:400] if bad is not None else ""),
            )
        )
    return _merge(out)


def _merge(results: list[TestResult]) -> list[TestResult]:
    """Parameterised cases share an id: the test passes only if every case passed."""
    merged: dict[str, TestResult] = {}
    for r in results:
        if r.id in merged:
            m = merged[r.id]
            m.passed = m.passed and r.passed
            m.seconds += r.seconds
            m.message = m.message or r.message
            m.verifies = sorted(set(m.verifies) | set(r.verifies))
        else:
            merged[r.id] = r
    return list(merged.values())


def run(out_dir: Path, system: bool = True) -> list[TestResult]:
    out_dir.mkdir(parents=True, exist_ok=True)
    gx = out_dir / "gtest.xml"
    subprocess.run([str(build_dir() / "iqk_tests"), f"--gtest_output=xml:{gx}"], capture_output=True, timeout=600)
    results = parse_gtest_xml(gx)
    px = out_dir / "pytest.xml"
    args = [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider", f"--junitxml={px}", "tests"]
    if not system:
        args += ["-m", "not system"]
    subprocess.run(args, cwd=ROOT, capture_output=True, timeout=3600, env={**os.environ, "IQK_BUILD": str(build_dir())})
    results += parse_pytest_xml(px)
    return results


def to_json(results: list[TestResult]) -> list[dict]:
    return [asdict(r) for r in results]
